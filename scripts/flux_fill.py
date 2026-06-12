import sys
sys.stdout.reconfigure(encoding='utf-8')  # Windows 'charmap' can't encode arrows/dashes
print("STATUS: Starting Python...", flush=True)

import os
import argparse
print("STATUS: Importing AI libraries (first run may take 30s)...", flush=True)

import torch
print("STATUS: Loading Hugging Face libraries...", flush=True)

from huggingface_hub import login, hf_hub_download
from huggingface_hub import model_info as hf_model_info
from huggingface_hub.utils import EntryNotFoundError, RepositoryNotFoundError, GatedRepoError
from diffusers import FluxInpaintPipeline, AutoencoderKL, Flux2KleinPipeline
from PIL import Image
import numpy as np
print("STATUS: Libraries loaded.", flush=True)

# ---------------------------------------------------------------------------
# Defaults (overridable via CLI args from settings)
# ---------------------------------------------------------------------------
DEFAULT_MODEL_ID  = "black-forest-labs/FLUX.2-klein-4B"
DEFAULT_VAE       = ""   # not needed for standard from_pretrained models
DEFAULT_TEXT_ENC  = ""   # not needed for standard from_pretrained models
DEFAULT_BASE_REPO = ""   # only used for fp8 single-file transformer loading


def parse_hf_path(hf_path):
    """Split 'owner/repo/path/to/file.ext' -> (repo_id, filename)."""
    parts = hf_path.split("/")
    return "/".join(parts[:2]), "/".join(parts[2:])


def _ensure_downloaded(repo_id, filename, target_path, token=None, size_hint=""):
    """Return target_path, downloading from HF first if missing."""
    if os.path.isfile(target_path):
        return target_path
    os.makedirs(os.path.dirname(target_path), exist_ok=True)
    local_dir = os.path.dirname(target_path)

    # Print periodic progress updates so the UI doesn't look frozen.
    import threading, time

    stop_flag = threading.Event()

    def _heartbeat():
        dots = 0
        while not stop_flag.wait(timeout=8):
            downloaded_mb = 0
            if os.path.isfile(target_path):
                downloaded_mb = os.path.getsize(target_path) / 1024 ** 2
            dots = (dots % 3) + 1
            hint = f" {size_hint}" if size_hint else ""
            print(f"STATUS: Downloading{hint} — {downloaded_mb:.0f} MB so far{'.' * dots}", flush=True)

    t = threading.Thread(target=_heartbeat, daemon=True)
    t.start()
    try:
        downloaded = hf_hub_download(repo_id=repo_id, filename=filename,
                                     local_dir=local_dir, token=token)
    finally:
        stop_flag.set()
        t.join(timeout=1)

    # Rename if hf_hub_download used a different name
    if os.path.abspath(downloaded) != os.path.abspath(target_path):
        import shutil
        shutil.move(downloaded, target_path)
    return target_path


def check_access(model_id, token=None):
    """
    Use the HF metadata API to verify repo access — no large file downloads.
    Returns True, "GATED", or an error string.
    """
    if token:
        print("STATUS: Logging in to Hugging Face...", flush=True)
        login(token=token)

    print(f"STATUS: Checking access to {model_id}...", flush=True)
    try:
        hf_model_info(model_id, token=token)
        print("STATUS: Access confirmed.", flush=True)
        return True
    except GatedRepoError:
        return "GATED"
    except RepositoryNotFoundError:
        # HF returns 404 instead of 403 for some gated repos when unauthenticated
        return "GATED"
    except Exception as e:
        return str(e)


def is_klein_model(model_id):
    return "klein" in model_id.lower()


def is_fp8_model(model_id):
    return "fp8" in model_id.lower()


def _patch_klein_inpainting(pipe):
    """Monkey-patch prepare_image_latents on a Flux2KleinPipeline instance so that
    a mask can be applied in latent space before the reference tokens are concatenated
    with the noise tokens inside the transformer.

    This replicates how ComfyUI's "Image Edit" node works: the reference image is
    encoded normally, then its latents are zeroed in the masked region.  Where the
    reference signal is zero the model has no image conditioning → the prompt drives
    generation freely there.  The surrounding unmasked latents remain intact, giving
    the model full context about the scene.

    Usage:
        pipe._inpaint_mask = torch.tensor (1, 1, H, W), float, 1=generate 0=keep
        pipe(image=original_image, ...)
        pipe._inpaint_mask = None
    """
    import types
    import torch.nn.functional as F

    def _masked_prepare_image_latents(self, images, batch_size, generator, device, dtype):
        image_latents_list = []
        for img in images:
            img = img.to(device=device, dtype=dtype)
            latent = self._encode_vae_image(image=img, generator=generator)
            # latent: (1, C, H_lat, W_lat) — already VAE-encoded and patchified
            mask = getattr(self, '_inpaint_mask', None)
            if mask is not None:
                h_lat, w_lat = latent.shape[2], latent.shape[3]
                m = mask.to(device=latent.device, dtype=latent.dtype)
                m = F.interpolate(m, size=(h_lat, w_lat), mode='nearest')
                latent = latent * (1.0 - m)   # zero reference where mask=1
            image_latents_list.append(latent)

        image_latent_ids = self._prepare_image_ids(image_latents_list)
        packed = [self._pack_latents(lat).squeeze(0) for lat in image_latents_list]
        image_latents = torch.cat(packed, dim=0).unsqueeze(0)
        image_latents = image_latents.repeat(batch_size, 1, 1)
        image_latent_ids = image_latent_ids.repeat(batch_size, 1, 1).to(device)
        return image_latents, image_latent_ids

    pipe.prepare_image_latents = types.MethodType(_masked_prepare_image_latents, pipe)
    pipe._inpaint_mask = None
    return pipe


def is_fast_model(model_id):
    """True for step-distilled models (4 steps, guidance ignored).
    FLUX.2-klein-base-* is NOT distilled — it uses 50 steps and real CFG."""
    name = model_id.lower()
    if "klein" in name:
        return "base" not in name   # klein-4B = distilled; klein-base-4B = not
    return "schnell" in name


class FluxWorker:
    def __init__(self, model_id, token=None, vae=DEFAULT_VAE, text_enc=DEFAULT_TEXT_ENC,
                 base_repo=DEFAULT_BASE_REPO,
                 transformer_path=None, vae_path=None, text_enc_path=None):
        print(f"STATUS: Loading Flux Model ({model_id})...", flush=True)
        if token:
            login(token=token)

        if is_klein_model(model_id) and is_fp8_model(model_id):
            self._load_klein_fp8(model_id, token, vae, text_enc, base_repo,
                                 transformer_path, vae_path, text_enc_path)
        elif is_klein_model(model_id):
            # Standard BF16 FLUX.2-klein (e.g. 4B/9B) — simple from_pretrained
            self.pipe = Flux2KleinPipeline.from_pretrained(
                model_id, torch_dtype=torch.bfloat16, token=token or None)
        elif is_fp8_model(model_id):
            try:
                self.pipe = FluxInpaintPipeline.from_pretrained(
                    model_id, torch_dtype=torch.float8_e4m3fn, token=token)
            except Exception:
                self.pipe = FluxInpaintPipeline.from_pretrained(
                    model_id, torch_dtype=torch.bfloat16, token=token)
        else:
            self.pipe = FluxInpaintPipeline.from_pretrained(
                model_id, torch_dtype=torch.bfloat16, token=token)

        # Patch klein pipelines to support latent-space inpainting (mask the reference tokens).
        if is_klein_model(model_id):
            _patch_klein_inpainting(self.pipe)

        if torch.cuda.is_available():
            if is_fp8_model(model_id):
                # fp8 upcasted to BF16: transformer alone is ~18.8 GB, all components ~27 GB.
                # Sequential offload (layer-by-layer) keeps peak VRAM to a single attention
                # block (~hundreds of MB) so nothing overflows the 24 GB RTX 3090.
                self.pipe.enable_sequential_cpu_offload()
                print("STATUS: Sequential GPU offload enabled (low VRAM mode).", flush=True)
            else:
                # BF16 klein-4B transformer is ~13 GB — fits in 24 GB with model-level offload.
                # Faster than sequential since whole components move at once, not layer-by-layer.
                self.pipe.enable_model_cpu_offload()
                print("STATUS: Model CPU offload enabled.", flush=True)
        # else: CPU-only — models remain in RAM, inference will be slow
        self._fast = is_fast_model(model_id)
        print("STATUS: Flux Ready", flush=True)

    def _load_klein_fp8(self, model_id, token, vae_path_setting, te_path_setting, base_repo,
                        transformer_path=None, vae_path=None, text_enc_path=None):
        """
        FLUX.2-klein fp8: only the transformer is loaded manually (it is a single fp8
        safetensors file that needs scalar-scale filtering before diffusers can load it).
        The VAE and text encoder are downloaded and loaded by the pipeline itself from
        base_repo — this guarantees the correct types (AutoencoderKLFlux2, Qwen3ForCausalLM)
        without the complexity of manual from_single_file loading for those components.
        """
        from diffusers.models import Flux2Transformer2DModel

        # Derive transformer filename: "black-forest-labs/FLUX.2-klein-9b-fp8"
        #   -> "flux-2-klein-9b-fp8.safetensors"
        transformer_file = model_id.split("/")[-1].lower().replace(".", "-") + ".safetensors"

        default_cache = os.path.expanduser("~/.cache/iqview/models")
        if not transformer_path:
            transformer_path = os.path.join(default_cache, transformer_file)

        print("STATUS: Downloading transformer (~9.4 GB, first run only — may take 10-20 min)...", flush=True)
        transformer_path = _ensure_downloaded(model_id, transformer_file, transformer_path,
                                              token=token, size_hint="transformer (~9.4 GB)")

        print("STATUS: Loading transformer (fp8 -> bf16, first run ~1-2 min)...", flush=True)
        # The fp8 safetensors file contains 0-dim F32 scalar scale tensors that confuse
        # diffusers' key-conversion (torch.chunk() on 0-dim tensors fails). Drop them.
        # FP8 weights are upcast to BF16 — native FP8 matmul requires Hopper (H100+),
        # not Ampere (RTX 3090), so we must run in BF16.
        from safetensors.torch import load_file as _load_safetensors
        raw_ckpt = _load_safetensors(transformer_path)
        clean_ckpt = {k: v for k, v in raw_ckpt.items()
                      if not (v.dim() == 0 and v.dtype == torch.float32)}
        del raw_ckpt

        # Must use Flux2Transformer2DModel (not FluxTransformer2DModel — that is FLUX.1).
        # config= must be explicit; without it from_single_file falls back to "FLUX.2-dev".
        transformer = Flux2Transformer2DModel.from_single_file(
            clean_ckpt,
            torch_dtype=torch.bfloat16,
            config=base_repo,
            subfolder="transformer",
            token=token,
        )
        del clean_ckpt

        # VAE and text encoder: let the pipeline download them from base_repo.
        # AutoencoderKLFlux2 and Qwen3ForCausalLM cannot be loaded via from_single_file
        # (not registered), so manual loading is not feasible. The pipeline handles the
        # correct types and weight shapes automatically.
        print("STATUS: Assembling pipeline (downloading VAE + text encoder on first run)...", flush=True)
        self.pipe = Flux2KleinPipeline.from_pretrained(
            base_repo,
            torch_dtype=torch.bfloat16,
            token=token,
            transformer=transformer,
        )

    def process(self, image_path, mask_path, prompt, output_path):
        image = Image.open(image_path).convert("RGB")
        mask  = Image.open(mask_path).convert("L")

        orig_size = image.size
        img_np    = np.array(image)
        mask_np   = np.array(mask)

        # 4 steps for distilled klein, 50 for non-distilled base.
        # guidance_scale=1.0 for distilled (CFG is disabled; any value > 1 is ignored
        # with a warning). guidance_scale=4.0 for the non-distilled base model.
        # These match the official BFL demo exactly.
        num_steps = 4   if self._fast else 50
        guidance  = 1.0 if self._fast else 4.0

        # Compute output dimensions: keep aspect ratio, cap to 1024×1024 total area,
        # round each side to the nearest multiple of 8. Matches the official demo.
        iw, ih = image.size
        scale  = min(1.0, (1024 * 1024 / (iw * ih)) ** 0.5)
        out_w  = (round(iw * scale / 8)) * 8
        out_h  = (round(ih * scale / 8)) * 8

        # For Flux2KleinPipeline: zero the reference image latents in the masked region
        # so the transformer has no image conditioning there → the prompt drives generation
        # freely in the mask, while unmasked regions stay fully conditioned on the original.
        # This is the same mechanism ComfyUI's "Image Edit" node uses.
        if hasattr(self.pipe, '_inpaint_mask'):
            mask_t = torch.from_numpy(mask_np.astype(np.float32) / 255.0)
            self.pipe._inpaint_mask = mask_t.unsqueeze(0).unsqueeze(0)  # (1,1,H,W)

        try:
            result = self.pipe(
                prompt=prompt,
                image=image,
                height=out_h,
                width=out_w,
                num_inference_steps=num_steps,
                guidance_scale=guidance,
                max_sequence_length=256,
            ).images[0]
        finally:
            if hasattr(self.pipe, '_inpaint_mask'):
                self.pipe._inpaint_mask = None   # always clear, even on exception

        # If the pipeline resized the image (>1024×1024 pixel limit), scale the result
        # back to the original dimensions before blending.
        if result.size != orig_size:
            result = result.resize(orig_size, Image.LANCZOS)

        # Blend: use generated pixels inside the mask region, keep originals outside.
        # Feather the mask (dilate, then blur) so the seam fades over ~15 px instead of
        # a 1-px hard edge — same treatment as the LaMa blend in worker.py. Dilating
        # first keeps alpha at 1.0 across the whole painted mask, so the fade happens
        # over generated content just outside it (which matches the original anyway).
        from PIL import ImageFilter
        feathered = mask.filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.GaussianBlur(6))
        result_np = np.array(result)
        alpha = np.array(feathered).astype(float) / 255.0
        alpha = alpha[..., np.newaxis]   # H×W×1 for broadcasting
        blended = (result_np * alpha + img_np * (1.0 - alpha)).clip(0, 255).astype(np.uint8)
        Image.fromarray(blended).save(output_path)
        print(f"OUTPUT: {output_path}", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check_only",        action="store_true")
    parser.add_argument("--token",             type=str, default=None)
    parser.add_argument("--model",             type=str, default=DEFAULT_MODEL_ID)
    parser.add_argument("--vae",               type=str, default=DEFAULT_VAE)
    parser.add_argument("--text_enc",          type=str, default=DEFAULT_TEXT_ENC)
    parser.add_argument("--base_repo",         type=str, default=DEFAULT_BASE_REPO)
    parser.add_argument("--transformer_path",  type=str, default=None)
    parser.add_argument("--vae_path",          type=str, default=None)
    parser.add_argument("--text_enc_path",     type=str, default=None)
    args = parser.parse_args()

    # Token arrives via HF_TOKEN env var (not on the command line, where it would be
    # visible in process listings). --token remains as a manual-testing fallback.
    if not args.token:
        args.token = os.environ.get("HF_TOKEN") or None

    # Diagnostic: log all received arguments so we can see exactly what C++ sent
    print(f"STATUS: args.model={args.model!r}", flush=True)
    print(f"STATUS: args.base_repo={args.base_repo!r}", flush=True)
    print(f"STATUS: args.vae={args.vae!r}", flush=True)
    print(f"STATUS: args.text_enc={args.text_enc!r}", flush=True)

    # Sanitize known stale legacy values. The "FLUX.2-dev" / "FLUX.1-dev" repos either
    # don't exist or aren't what we want — if anything matches, snap back to the klein defaults.
    def _looks_legacy(value):
        if not value:
            return True
        v = value.lower()
        return ("flux.2-dev" in v) or ("flux.1-dev" in v) or ("flux-2-dev" in v) \
            or ("flux.2-klein-9b-fp8" in v)

    if _looks_legacy(args.model):
        if args.model:
            print(f"STATUS: Correcting stale model ID '{args.model}' -> '{DEFAULT_MODEL_ID}'", flush=True)
        args.model = DEFAULT_MODEL_ID
    if _looks_legacy(args.base_repo):
        if args.base_repo:
            print(f"STATUS: Correcting stale base repo '{args.base_repo}' -> '{DEFAULT_BASE_REPO}'", flush=True)
        args.base_repo = DEFAULT_BASE_REPO
    if _looks_legacy(args.vae):
        args.vae = DEFAULT_VAE
    if _looks_legacy(args.text_enc):
        args.text_enc = DEFAULT_TEXT_ENC

    if args.check_only:
        status = check_access(args.model, args.token)
        if status is True:
            print("ACCESS_GRANTED", flush=True)
        elif status == "GATED":
            print("ACCESS_GATED", flush=True)
        else:
            print(f"ERROR: {status}", flush=True)
        return

    try:
        worker = FluxWorker(args.model, args.token, args.vae, args.text_enc, args.base_repo,
                            transformer_path=args.transformer_path,
                            vae_path=args.vae_path,
                            text_enc_path=args.text_enc_path)
    except Exception as e:
        print(f"FATAL: {e}", flush=True)
        sys.exit(1)

    while True:
        try:
            line = sys.stdin.readline()
            if not line:
                break
            parts = line.strip().split("|")
            if len(parts) >= 4:
                worker.process(parts[0], parts[1], parts[2], parts[3])
            else:
                print("ERROR: Invalid command format. Expected img|mask|prompt|out", flush=True)
        except Exception as e:
            print(f"ERROR: {e}", flush=True)


if __name__ == "__main__":
    main()
