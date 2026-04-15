import os
import sys
import argparse
import numpy as np
import cv2
import urllib.request
from pathlib import Path

# Try to import onnxruntime
try:
    import onnxruntime as ort
except ImportError:
    print("Error: onnxruntime not found. Please install with 'pip install onnxruntime-gpu'")
    sys.exit(1)

MODEL_URL = "https://huggingface.co/anyisalin/big-lama-onnx/resolve/main/onnx/model.onnx"
MODEL_PATH = Path(__file__).parent / "model.onnx"

def download_model():
    if not MODEL_PATH.exists():
        # Using stderr for status so it doesn't pollute stdout if needed, 
        # but we'll stick to print for now as C++ reads both.
        sys.stderr.write(f"Downloading model (200MB) to {MODEL_PATH}...\n")
        opener = urllib.request.build_opener()
        opener.addheaders = [('User-agent', 'Mozilla/5.0')]
        urllib.request.install_opener(opener)
        urllib.request.urlretrieve(MODEL_URL, MODEL_PATH)
        sys.stderr.write("Download complete.\n")

def pad_img(img, pad_factor=8):
    h, w = img.shape[:2]
    pad_h = (pad_factor - h % pad_factor) % pad_factor
    pad_w = (pad_factor - w % pad_factor) % pad_factor
    img = np.pad(img, ((0, pad_h), (0, pad_w), (0, 0)), mode='reflect')
    return img, pad_h, pad_w

def inpaint(image_path, mask_path, output_path):
    download_model()

    # Load image and mask
    img = cv2.imread(image_path)
    if img is None: raise ValueError(f"Could not load image {image_path}")
    img_orig = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    
    mask_bgr = cv2.imread(mask_path)
    if mask_bgr is None: raise ValueError(f"Could not load mask {mask_path}")
    mask = np.max(mask_bgr, axis=2)
    
    # Process mask
    _, mask = cv2.threshold(mask, 10, 255, cv2.THRESH_BINARY)
    
    # Pad/Resize to 512x512
    orig_h, orig_w = img_orig.shape[:2]
    img_512 = cv2.resize(img_orig, (512, 512), interpolation=cv2.INTER_AREA)
    mask_512 = cv2.resize(mask, (512, 512), interpolation=cv2.INTER_NEAREST)

    # Normalize
    img_512 = img_512.astype(np.float32) / 255.0
    mask_512 = mask_512.astype(np.float32) / 255.0
    mask_512 = (mask_512 > 0.5).astype(np.float32) # Ensure binary [0, 1]
    mask_512 = mask_512[None, None, :, :] 

    # Prepare input for LaMa
    img_512 = img_512.transpose(2, 0, 1)[None, :, :, :] 

    # Initialize ONNX session
    so = ort.SessionOptions()
    so.log_severity_level = 3
    
    providers = ['CUDAExecutionProvider', 'CPUExecutionProvider']
    session = ort.InferenceSession(str(MODEL_PATH), sess_options=so, providers=providers)
    
    # Use dynamic names to be 100% sure
    input_names = [i.name for i in session.get_inputs()]
    # Typically ['l_image_', 'l_mask_']
    inputs = {
        input_names[0]: img_512,
        input_names[1]: mask_512
    }
    
    # Run inference
    result = session.run(None, inputs)[0]
    
    # Post-process
    result = result[0].transpose(1, 2, 0)
    if result.max() <= 1.01:
        result = np.clip(result * 255, 0, 255).astype(np.uint8)
    else:
        result = np.clip(result, 0, 255).astype(np.uint8)
    
    # Resize back to original
    result = cv2.resize(result, (orig_w, orig_h), interpolation=cv2.INTER_LANCZOS4)
    
    # RE-ENABLING BLENDING: Only apply result to the MASKED area 
    # This keeps the rest of the image at full, original resolution
    mask_blurred = cv2.GaussianBlur(mask, (7, 7), 0)
    mask_alpha = mask_blurred.astype(np.float32) / 255.0
    mask_alpha = mask_alpha[:, :, None]
    
    final = (result.astype(np.float32) * mask_alpha + img_orig.astype(np.float32) * (1.0 - mask_alpha))
    final = np.clip(final, 0, 255).astype(np.uint8)
    
    # Convert back to BGR for saving
    final_bgr = cv2.cvtColor(final, cv2.COLOR_RGB2BGR)
    cv2.imwrite(output_path, final_bgr)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True)
    parser.add_argument("--mask", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    try:
        inpaint(args.image, args.mask, args.output)
        sys.exit(0)
    except Exception as e:
        sys.stderr.write(f"RET_ERR: {str(e)}\n")
        sys.exit(1)
