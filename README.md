<h1 align=center>iqView</h1>

<p align=center>iqView is a powerful fork of <b>qView</b> that integrates local, GPU-accelerated AI tools for rapid image editing without sacrificing minimalism.</p>

<h3 align=center>
    Retouch | Creative Fill | Isolate | Local AI
</h3>

<p align=center>
    <img alt="Screenshot" src="https://interversehq.com/qview/assets/img/screenshot3.png">
</p>

## ✨ What is iqView?
While original qView is a fantastic minimalist viewer, **iqView** expands it into a lightweight creative toolkit powered by local AI on your CUDA-capable GPU (developed on an NVIDIA RTX 3090).

## 🛠 AI Features
- **Object Removal (R)** — *LaMa inpainting*: mask distracting objects, text, or photobombers and they vanish in a fraction of a second. Brush and lasso masking tools included.
- **Creative Fill (G)** — *FLUX.2 klein*: mask a region, type a prompt, and generate photo-real replacement content in seconds.
- **Isolate (S)** — *SAM 3 segmentation*: automatically segment the image, pick the segments to keep, and get a transparent-background cutout.
- **Zero Configuration**: the Python environment and AI models are set up and downloaded automatically on first use.
- **Privacy First**: all AI processing happens locally on your machine — your photos never leave your computer. (Gated models like FLUX and SAM 3 require a free Hugging Face token to download weights.)

## 🎮 Shortcuts
| Key | Action | Engine |
| --- | --- | --- |
| **R** | Toggle Retouch Mode (Cycle: Brush -> Lasso -> Off) / Apply if masked | LaMa |
| **G** | Creative Fill: mask + prompt | FLUX.2 klein |
| **S** | Isolate: segment and cut out subjects | SAM 3 |
| **Enter** | Apply Retouch / Confirm | - |
| **Esc** | Cancel / Exit Mode | - |
| **Middle Click** | Apply Retouch (Quick Action) | - |
| **Right Click** | Cancel Retouch (Quick Action) | - |
| **[ / ]** | Adjust Brush Size | - |
| **Ctrl+Z** | Undo / Compare | - |
| **Ctrl+F** | Flip Image | - |

## Installation
Requires **Python 3.10+** on your PATH; iqView creates its own virtual environment on first use. An NVIDIA GPU with CUDA is strongly recommended (CPU fallback works but is slow).
C++ code builds with **Qt 6 / CMake** on Windows, Linux, and macOS.

---
*Based on the original [qView](https://github.com/jurplel/qView) by Jurplel.*
