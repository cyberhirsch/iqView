# Flux.1 Generative Fill Integration Plan

This document outlines the implementation for the "Creative Fill" feature using the Flux.1 model.

## 1. Hugging Face Authentication Flow (Smooth User Journey)
Since Flux.1 models are often gated, we will implement a "Smart Auth" system:
- **Detection**: The first time Creative Fill is used, iqView will attempt to check model access.
- **On-Demand Dialog**: If access is denied (403), iqView will show a premium dialog:
  - Instructions on how to visit the HF model page and click "Agree".
  - A field to paste an HF Access Token (Stored securely in `QSettings`).
  - A "Verify & Continue" button.

## 2. The Creative Fill UI
- **Prompt Input**: A sleek, semi-transparent text bar at the bottom of the screen when in "Creative" mode.
- **Brush/Lasso**: Reuses the existing retouching masks.
- **Strength Slider**: Controls how much the AI adheres to the original pixels.

## 3. High-Performance Backend (`scripts/flux_fill.py`)
- **Optimization**: Use `diffusers` with `torch` and `bitsandbytes` (4-bit/8-bit quantization) to ensure the 9B model fits comfortably in the 3090's 24GB VRAM.
- **Pipeline**:
    1. Load oriented image/mask.
    2. Run `FluxInpaintPipeline`.
    3. Blended post-processing (similar to LaMa but with higher fidelity).

## 4. Integration Steps
1. [ ] Update `MainWindow` to show the Generative Text Input.
2. [ ] Create `scripts/flux_fill.py` based on `diffusers`.
3. [ ] Implement `HFAuthDialog` in C++.
4. [ ] Extend `worker.py` (or create a separate `flux_worker.py`) to manage the large model lifecycle.

---
**Priority 1**: Implement the C++ Auth Dialog and the Python access check.
**Priority 2**: The prompt-based inference engine.
