# iqView Retouch & AI Expansion Plan

This document outlines the current state and future roadmap for integrating AI-powered image editing into qView, leveraging the local RTX 3090 GPU.

## 🛠 Project Progress

### Phase 1: Core Framework (Completed)
- [x] **Action Registration**: `R` key registered for retouch mode.
- [x] **Context Menu**: Added "Retouch" entry.
- [x] **Python Bridge**: `QProcess` bridge with auto-model downloading.
- [x] **Performance**: LaMa model inference optimized for RTX 3090.

### Phase 2: Masking & UI (Completed)
- [x] **Coordinated Mask**: Mask item child-parent relationship fixed for pan/zoom sync.
- [x] **Brush Tool**: Variable size brushing with real-time preview.
- [x] **Lasso Tool**: Polygon selection for large area removal.
- [x] **Mouse Controls**: 
    - Middle Mouse -> Confirm/Apply
    - Right Mouse -> Cancel/Exit

### Phase 3: AI Backend (Completed)
- [x] **Inference**: ONNX Runtime optimization for sub-second cleanup.
- [x] **Undo/Redo**: `Ctrl+Z` logic for instant comparison with original image.
- [x] **Blending Logic**: Multi-pass Gaussian feathering for seamless integration.
- [x] **Accuracy**: Fixed mask positioning and coordinate sync issues.

---

## 🚀 Roadmap: High-Quality Creative Editing

The next major milestone is implementing **Flux Fill**, a state-of-the-art diffusion inpainting model that provides significantly better quality and creative control than traditional inpainters.

### Phase 4: Creative Fill & Expansion (Planned)
- [ ] **Action Registration**: Add `F` key for "Flux AI Fill".
- [ ] **Model Engine**: Implement `scripts/flux_fill.py` using **black-forest-labs/FLUX.2-klein-9B**.
- [ ] **Ultra-Fast Inference**: Leverage the Klein architecture for sub-second, multi-reference editing.
- [ ] **Prompt Interface**: Add a simple text entry overlay for descriptive fill (e.g., "majestic mountain background").
- [ ] **VRAM Management**: Implement dynamic offloading logic for the 24GB RTX 3090.

## 🛠 Current Controls Summary

| Key / Mouse | Action |
| --- | --- |
| **R** | Toggle Mode (Cycle: Brush -> Lasso -> Off) |
| **F** | (Future) AI Prompt Fill |
| **Middle Click** | Apply / Confirm |
| **Right Click** | Cancel / Exit |
| **[ / ]** | Increase / Decrease Brush Size |
| **Enter** | Apply / Confirm |
| **Esc** | Cancel / Exit |

## Technical Stack
- **UI**: C++ (Qt)
- **AI Backend**: Python 3.10+
- **Inference**: ONNX Runtime (CUDA) / PyTorch
- **Hardware Target**: NVIDIA RTX 3090
