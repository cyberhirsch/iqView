# Inpainting Model Comparison for qView Retouch

This document outlines various AI models that can be used for the retouching feature and their respective strengths. Since you are using an **RTX 3090**, most of these will run very efficiently.

| Model | Size | Speed (3090) | Capabilities |
| :--- | :--- | :--- | :--- |
| **LaMa (Current)** | 200MB | ~100-200ms | Fast, clean object removal, high-res support. |
| **Flux 2 Klein** | 9B (Quantized) | < 1.0s | Flagship speed, photo-real creative fill. |
| **SDXL Inpaint** | ~6GB | ~1-2s | Good balance, widely supported. |

## 🧠 Model Details

### 1. LaMa (Large Mask Inpainting) - *Active*
Our current default for fast retouching. It is specialized for "Retouching" where the goal is to make an object disappear by filling it with plausible background textures (sky, grass, wall). It is perfect for qView because it's nearly instantaneous.

### 2. Flux 2 Klein (9B) - *Active*
State-of-the-art foundational small model. Released in 2026, Flux 2 Klein is optimized for real-time interactive editing. It can "imagine" new things with flagship fidelity while maintaining sub-second inference on the RTX 3090.

## Recommended Choice: LaMa
For **qView**, **LaMa** is the superior choice because:
1. **Speed**: It feels like a native tool rather than a slow "AI plugin."
2. **Resolution Robustness**: It handles images of any size/aspect ratio without needing internal cropping/scaling artifacts.
3. **No Prompt Required**: It works purely on visual context, which fits a minimal image viewer perfectly.

## Future Possibilities
If we want to expand the "features" mentioned in your request, we could add a **"Creative Mode"** using **SD-XL Turbo**, which would allow you to not just remove objects, but replace them by typing a prompt.
