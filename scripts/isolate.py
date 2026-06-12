import sys
sys.stdout.reconfigure(encoding='utf-8')
print("STATUS: Starting segmentation worker...", flush=True)

import os
import argparse
import numpy as np
from PIL import Image
import torch
print("STATUS: Loading segmentation libraries...", flush=True)
from transformers import pipeline as hf_pipeline
from huggingface_hub import login
from huggingface_hub import model_info as hf_model_info
from huggingface_hub.utils import GatedRepoError, RepositoryNotFoundError
print("STATUS: Libraries loaded.", flush=True)

SAM3_MODEL_ID = "facebook/sam3"

# Distinct vibrant colors for up to 32 segments
SEGMENT_COLORS = [
    (255,  59,  48),   #  1 red
    (  0, 122, 255),   #  2 blue
    ( 52, 199,  89),   #  3 green
    (255, 149,   0),   #  4 orange
    (175,  82, 222),   #  5 purple
    (255,  45,  85),   #  6 pink
    ( 90, 200, 250),   #  7 cyan
    (255, 204,   0),   #  8 yellow
    (162, 132,  94),   #  9 brown
    ( 88,  86, 214),   # 10 indigo
    ( 48, 209, 163),   # 11 mint
    (255,  99,  48),   # 12 coral
    (100, 210, 255),   # 13 sky
    (191,  90, 242),   # 14 violet
    ( 50, 173, 230),   # 15 azure
    (255, 179,  64),   # 16 amber
    (255,  55, 148),   # 17 magenta
    (  0, 199, 190),   # 18 turquoise
    (255, 214,  10),   # 19 gold
    (142, 142, 147),   # 20 gray
    (174, 214, 241),   # 21 light blue
    (171, 235, 198),   # 22 light green
    (250, 215, 160),   # 23 peach
    (215, 189, 226),   # 24 lavender
    (245, 183, 177),   # 25 salmon
    (210, 180, 140),   # 26 tan
    (152, 255, 152),   # 27 lime
    (255, 128,   0),   # 28 dark orange
    (  0, 150, 136),   # 29 teal
    (233,  30,  99),   # 30 deep pink
    ( 63,  81, 181),   # 31 dark blue
    (139, 195,  74),   # 32 light green 2
]


def check_access(token=None):
    """Verify SAM 3 repo access without downloading weights. Returns True, 'GATED', or error str."""
    if token:
        login(token=token)
    try:
        hf_model_info(SAM3_MODEL_ID, token=token)
        return True
    except Exception as e:
        msg = str(e).lower()
        # Catch gated-repo signals regardless of exact exception class or HF version
        if ("gated" in msg or "403" in msg
                or ("access" in msg and "repo" in msg)
                or "accept" in msg):
            return "GATED"
        # Explicit class check as a secondary path (may not exist in older HF versions)
        try:
            if isinstance(e, (GatedRepoError, RepositoryNotFoundError)):
                return "GATED"
        except Exception:
            pass
        return str(e)


class IsolateWorker:
    def __init__(self, token=None):
        self.pipe   = None
        self.token  = token
        self._masks = []   # list of (H, W) bool numpy arrays, one per segment

    def _load_model(self):
        print("STATUS: Loading SAM 3 model (~3.2 GB on first run)...", flush=True)
        if self.token:
            login(token=self.token)
        device = 0 if torch.cuda.is_available() else "cpu"
        self.pipe = hf_pipeline(
            "mask-generation",
            model=SAM3_MODEL_ID,
            device=device,
            token=self.token or None,
        )
        print("STATUS: SAM 3 ready.", flush=True)

    def segment(self, image_path, viz_path, idmap_path):
        """Segment image, write colorized viz + 8-bit id map. Returns segment count."""
        image = Image.open(image_path).convert("RGB")
        W, H = image.size

        # Scale to max 1024 px on the longer side to keep inference fast
        scale = min(1.0, 1024.0 / max(W, H))
        run_w = max(8, round(W * scale / 8) * 8)
        run_h = max(8, round(H * scale / 8) * 8)
        run_image = image.resize((run_w, run_h), Image.LANCZOS) if scale < 1.0 else image

        print("STATUS: Segmenting image with SAM 3...", flush=True)
        if self.pipe is None:
            self._load_model()

        outputs = self.pipe(run_image, points_per_batch=64)
        raw_masks = outputs.get("masks", [])

        # Normalize mask format (some versions return list of dicts)
        if raw_masks and isinstance(raw_masks[0], dict):
            raw_masks = [m["segmentation"] for m in raw_masks]

        # Sort smallest-area first so specific (small) segments take priority when
        # assigning non-overlapping pixels
        raw_masks = sorted(raw_masks, key=lambda m: np.asarray(m).sum())

        # Build non-overlapping label map (0 = background/unassigned)
        label_map = np.zeros((run_h, run_w), dtype=np.uint8)
        min_area = max(100, run_w * run_h // 500)   # ignore tiny noise masks
        n = 0

        for raw in raw_masks:
            mask = np.asarray(raw, dtype=bool)
            if mask.shape != (run_h, run_w):
                continue
            # Only claim pixels that aren't yet assigned
            unassigned = (label_map == 0) & mask
            if unassigned.sum() < min_area:
                continue
            n += 1
            if n > len(SEGMENT_COLORS):
                break
            label_map[unassigned] = n

        # Scale label map back to original image resolution
        if scale < 1.0:
            from PIL import Image as _PIL
            lbl_pil = _PIL.fromarray(label_map, mode='L').resize((W, H), Image.NEAREST)
            label_map = np.array(lbl_pil)

        # Store per-segment boolean masks at original resolution
        self._masks = [(label_map == i) for i in range(1, n + 1)]

        # Save id map
        Image.fromarray(label_map).save(idmap_path)

        # Build colorized visualization
        img_np = np.array(image, dtype=np.uint8)
        overlay = np.zeros_like(img_np)
        for i, color in enumerate(SEGMENT_COLORS[:n], start=1):
            overlay[label_map == i] = color

        # 55% color, 45% original for segments; 50% darkness for unclassified
        result = (0.55 * overlay + 0.45 * img_np).clip(0, 255).astype(np.uint8)
        result[label_map == 0] = (img_np[label_map == 0] * 0.45).astype(np.uint8)
        Image.fromarray(result).save(viz_path)

        print(f"STATUS: Found {n} segments.", flush=True)
        return n

    def compose(self, image_path, selected_ids, output_path):
        """Combine selected segment masks into a PNG with alpha transparency."""
        image = Image.open(image_path).convert("RGBA")
        W, H = image.size

        alpha = np.zeros((H, W), dtype=np.uint8)
        for idx in selected_ids:
            if 0 <= idx < len(self._masks):
                m = self._masks[idx]
                if m.shape == (H, W):
                    alpha[m] = 255

        result = np.array(image, dtype=np.uint8)
        result[:, :, 3] = alpha
        Image.fromarray(result).save(output_path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--token", type=str, default=None)
    args = parser.parse_args()

    # Token arrives via HF_TOKEN env var (not on the command line, where it would be
    # visible in process listings). --token remains as a manual-testing fallback.
    if not args.token:
        args.token = os.environ.get("HF_TOKEN") or None

    # Verify access before loading the model so C++ can show the auth dialog.
    status = check_access(args.token)
    if status == "GATED":
        print("ACCESS_GATED", flush=True)
        return
    elif status is not True:
        print(f"FATAL: {status}", flush=True)
        return

    worker = IsolateWorker(token=args.token)

    while True:
        try:
            line = sys.stdin.readline()
            if not line:
                break
            parts = line.strip().split("|")
            cmd = parts[0] if parts else ""

            if cmd == "SEGMENT" and len(parts) >= 4:
                image_path, viz_path, idmap_path = parts[1], parts[2], parts[3]
                n = worker.segment(image_path, viz_path, idmap_path)
                print(f"SEGMENTS: {n}", flush=True)

            elif cmd == "COMPOSE" and len(parts) >= 4:
                image_path = parts[1]
                ids_str    = parts[2]
                output_path = parts[3]
                selected = [int(x) for x in ids_str.split(",") if x.strip().isdigit()]
                worker.compose(image_path, selected, output_path)
                print(f"OUTPUT: {output_path}", flush=True)

            else:
                if cmd:
                    print(f"ERROR: Unknown command: {cmd}", flush=True)

        except Exception as e:
            import traceback
            print(f"ERROR: {e}", flush=True)
            traceback.print_exc(file=sys.stderr)


if __name__ == "__main__":
    main()
