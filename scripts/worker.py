import os
import sys
import numpy as np
import cv2
import onnxruntime as ort
import time

def log(msg):
    # Log to a fixed file for debugging the persistent worker
    temp_dir = os.environ.get('TEMP', '/tmp')
    log_path = os.path.join(temp_dir, "iqview_worker_log.txt")
    with open(log_path, "a") as f:
        f.write(f"[{time.ctime()}] {msg}\n")
    # Don't print to stdout as it's used for communication


def _download_model_if_needed(model_path):
    """Download big-lama.onnx from HuggingFace if not already present."""
    if os.path.isfile(model_path):
        return
    print("STATUS: LaMa model not found. Downloading (~200 MB, one-time)...", flush=True)
    target_dir = os.path.dirname(model_path) or "."
    os.makedirs(target_dir, exist_ok=True)
    try:
        from huggingface_hub import hf_hub_download
        import shutil
        downloaded = hf_hub_download(
            repo_id="anyisalin/big-lama-onnx",
            filename="onnx/model.onnx",
            local_dir=target_dir,
        )
        if os.path.abspath(downloaded) != os.path.abspath(model_path):
            shutil.move(downloaded, model_path)
        print("STATUS: Download complete.", flush=True)
    except Exception as e:
        print(f"FATAL: Failed to download LaMa model: {e}", flush=True)
        sys.exit(1)


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=str, default=None,
                        help="Path to big-lama.onnx. Defaults to big-lama.onnx next to this script.")
    args = parser.parse_args()

    log("Persistent Worker Starting...")

    # Identify model path
    script_dir = os.path.dirname(os.path.abspath(__file__))
    model_path = args.model if args.model else os.path.join(script_dir, "big-lama.onnx")

    # Auto-download if missing
    _download_model_if_needed(model_path)

    print("STATUS: Loading LaMa model...", flush=True)

    # Device detection
    available = ort.get_available_providers()
    requested = ['DmlExecutionProvider', 'CUDAExecutionProvider']
    providers = [p for p in requested if p in available] + ['CPUExecutionProvider']
    
    sess_options = ort.SessionOptions()
    sess_options.log_severity_level = 3
    sess_options.enable_mem_pattern = False
    
    log(f"Loading model with providers: {providers}")
    try:
        session = ort.InferenceSession(model_path, providers=providers, sess_options=sess_options)
        log("Model loaded successfully.")
    except Exception as e:
        log(f"FATAL: Model load failed: {e}")
        print(f"FATAL: {e}")
        sys.exit(1)

    print("READY")
    sys.stdout.flush()

    while True:
        line = sys.stdin.readline()
        if not line:
            break
        
        line = line.strip()
        if not line:
            continue
            
        log(f"Received command: {line}")
        
        try:
            # Command format: image_path|mask_path|output_path
            parts = line.split('|')
            if len(parts) != 3:
                print("ERROR: Invalid command format. Use image|mask|output")
                sys.stdout.flush()
                continue
                
            img_path, mask_path, out_path = parts
            
            # 1. Load
            img = cv2.imread(img_path)
            mask = cv2.imread(mask_path, 0)
            
            if img is None or mask is None:
                print("ERROR: Could not load input files.")
                sys.stdout.flush()
                continue
                
            # 2. Extract ROI (reused logic from retoucher.py)
            _, mask_bin = cv2.threshold(mask, 1, 255, cv2.THRESH_BINARY)
            coords = cv2.findNonZero(mask_bin)
            if coords is None:
                cv2.imwrite(out_path, img)
                print("DONE")
                sys.stdout.flush()
                continue
                
            x, y, w, h = cv2.boundingRect(coords)
            pad = 128
            y1 = max(0, y - pad)
            y2 = min(img.shape[0], y + h + pad)
            x1 = max(0, x - pad)
            x2 = min(img.shape[1], x + w + pad)
            roi_img = img[y1:y2, x1:x2]
            roi_mask = mask_bin[y1:y2, x1:x2]
            
            # 3. Prep — keep aspect ratio (squashing the ROI to a square distorts
            # textures during inference), then edge-pad to the model's 512x512.
            target_size = 512
            roi_h, roi_w = roi_img.shape[:2]
            ar_scale = target_size / max(roi_h, roi_w)
            scaled_w = max(8, min(target_size, int(round(roi_w * ar_scale))))
            scaled_h = max(8, min(target_size, int(round(roi_h * ar_scale))))
            pad_r = target_size - scaled_w
            pad_b = target_size - scaled_h

            img_prep = cv2.cvtColor(roi_img, cv2.COLOR_BGR2RGB)
            img_prep = cv2.resize(img_prep, (scaled_w, scaled_h), interpolation=cv2.INTER_AREA)
            img_prep = cv2.copyMakeBorder(img_prep, 0, pad_b, 0, pad_r, cv2.BORDER_REPLICATE)
            mask_prep = cv2.resize(roi_mask, (scaled_w, scaled_h), interpolation=cv2.INTER_NEAREST)
            mask_prep = cv2.copyMakeBorder(mask_prep, 0, pad_b, 0, pad_r, cv2.BORDER_CONSTANT, value=0)
            
            t_img = img_prep.transpose(2, 0, 1).astype(np.float32) / 255.0
            t_mask = mask_prep[np.newaxis, ...].astype(np.float32) / 255.0
            t_img = t_img * (1.0 - t_mask)
            t_img = t_img[np.newaxis, ...]
            t_mask = t_mask[np.newaxis, ...]
            
            inputs = {
                session.get_inputs()[0].name: t_img,
                session.get_inputs()[1].name: t_mask
            }
            
            # 4. Inference with fallback
            try:
                res_raw = session.run(None, inputs)[0][0]
            except Exception as e:
                log(f"GPU Failed: {e}. Falling back to CPU...")
                cpu_sess = ort.InferenceSession(model_path, providers=['CPUExecutionProvider'], sess_options=sess_options)
                res_raw = cpu_sess.run(None, inputs)[0][0]

            if res_raw.max() <= 1.0:
                res_raw = res_raw * 255.0
            
            # 5. Post — crop away the padding before scaling back to ROI size
            res_img = res_raw.transpose(1, 2, 0).astype(np.uint8)
            res_img = cv2.cvtColor(res_img, cv2.COLOR_RGB2BGR)
            res_img = res_img[:scaled_h, :scaled_w]
            res_full = cv2.resize(res_img, (roi_w, roi_h), interpolation=cv2.INTER_CUBIC)
            
            # Blending
            mask_alpha = roi_mask.astype(float) / 255.0
            mask_alpha = cv2.dilate(mask_alpha, np.ones((5, 5), np.uint8), iterations=2)
            mask_alpha = cv2.GaussianBlur(mask_alpha, (15, 15), 0)
            mask_alpha = mask_alpha[..., np.newaxis]
            
            final_roi = (res_full.astype(float) * mask_alpha + roi_img.astype(float) * (1.0 - mask_alpha)).astype(np.uint8)
            
            final_img = img.copy()
            final_img[y1:y2, x1:x2] = final_roi
            
            cv2.imwrite(out_path, final_img)
            print("DONE")
            sys.stdout.flush()
            log("Job finished successfully.")
            
        except Exception as e:
            log(f"ERROR during job: {e}")
            print(f"ERROR: {e}")
            sys.stdout.flush()

if __name__ == "__main__":
    main()
