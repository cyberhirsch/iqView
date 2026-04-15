import os
import sys
import argparse
from huggingface_hub import login, hf_hub_download
from diffusers import FluxInpaintPipeline
import torch

def check_access(model_id, token=None):
    if token:
        login(token=token)
    try:
        # Just try to download the config to check access
        hf_hub_download(repo_id=model_id, filename="config.json", token=token)
        return True
    except Exception as e:
        if "403" in str(e) or "Gated" in str(e):
            return "GATED"
        return str(e)

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

    # Inpaint logic will go here
    print("READY (Flux Placeholder)")

if __name__ == "__main__":
    main()
