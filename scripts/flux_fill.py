import os
import sys
import argparse
import torch
from huggingface_hub import login, hf_hub_download
from diffusers import FluxInpaintPipeline
from PIL import Image
import numpy as np

def check_access(model_id, token=None):
    if token:
        login(token=token)
    try:
        # Check access by attempting to download config
        hf_hub_download(repo_id=model_id, filename="config.json", token=token)
        return True
    except Exception as e:
        if "403" in str(e) or "Gated" in str(e):
            return "GATED"
        return str(e)

class FluxWorker:
    def __init__(self, model_id, token=None):
        print(f"STATUS: Loading Flux Model ({model_id})...")
        if token:
            login(token=token)
            
        # Use float16 for memory efficiency on 24GB VRAM (3090)
        self.pipe = FluxInpaintPipeline.from_pretrained(
            model_id, 
            torch_dtype=torch.bfloat16, # Flux prefers bfloat16
            token=token
        )
        
        # Optimize for 3090
        # enable_model_cpu_offload() is safer for 24GB VRAM with Flux
        self.pipe.enable_model_cpu_offload() 
        # self.pipe.to("cuda") # Try direct to CUDA if offload is too slow
        
        print("STATUS: Flux Ready")

    def process(self, image_path, mask_path, prompt, output_path):
        image = Image.open(image_path).convert("RGB")
        mask = Image.open(mask_path).convert("L")
        
        # Flux Inpaint logic
        # Schnell needs ~4 steps, Dev needs ~20-50
        num_steps = 4 if "schnell" in self.pipe.config._name_or_path.lower() else 30
        
        result = self.pipe(
            prompt=prompt,
            image=image,
            mask_image=mask,
            width=image.width,
            height=image.height,
            num_inference_steps=num_steps,
            guidance_scale=0.0, # Schnell doesn't use guidance
            max_sequence_length=256
        ).images[0]
        
        result.save(output_path)
        print(f"OUTPUT: {output_path}")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check_only", action="store_true")
    parser.add_argument("--token", type=str, default=None)
    parser.add_argument("--model", type=str, default="black-forest-labs/FLUX.1-schnell")
    args = parser.parse_args()

    if args.check_only:
        status = check_access(args.model, args.token)
        if status == True:
            print("ACCESS_GRANTED")
        elif status == "GATED":
            print("ACCESS_GATED")
        else:
            print(f"ERROR: {status}")
        return

    # Worker Mode
    worker = None
    try:
        worker = FluxWorker(args.model, args.token)
    except Exception as e:
        print(f"FATAL: {str(e)}")
        sys.exit(1)

    while True:
        try:
            line = sys.stdin.readline()
            if not line:
                break
            
            parts = line.strip().split("|")
            if len(parts) >= 4:
                img_p, msk_p, pmt, out_p = parts[0], parts[1], parts[2], parts[3]
                worker.process(img_p, msk_p, pmt, out_p)
            else:
                print("ERROR: Invalid command format. Expected img|mask|prompt|out")
        except Exception as e:
            print(f"ERROR: {str(e)}")

if __name__ == "__main__":
    main()
