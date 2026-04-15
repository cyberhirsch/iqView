<h1 align=center>iqView</h1>

<p align=center>iqView is a powerful fork of <b>qView</b> that integrates local, GPU-accelerated AI tools for rapid image editing without sacrificing minimalism.</p>

<h3 align=center>
    Retouch | Object Removal | Local AI
</h3>

<p align=center>
    <img alt="Screenshot" src="https://interversehq.com/qview/assets/img/screenshot3.png">
</p>

## ✨ What is iqView?
While original qView is a fantastic minimalist viewer, **iqView** expands it into a lightweight creative toolkit. 

Featuring built-in **LaMa (Large Mask Inpainting)**, you can instantly removed distracting objects, text, or photobombers directly from the viewer using your **NVIDIA RTX 3090** or other CUDA-capable GPUs.

## 🛠 AI Features
- **Object Removal (R)**: Instantly mask and remove items.
- **Multi-Tool Masking**: Switch between a **Brush** and a **Lasso** for precision.
- **Zero Configuration**: AI models are downloaded automatically on the first run.
- **Privacy First**: All AI processing happens locally on your machine—your photos never leave your computer.

## 🎮 Shortcuts
| Key | Action |
| --- | --- |
| **R** | Toggle Retouch Mode (Cycle: Brush -> Lasso -> Off) |
| **Enter** | Apply Retouch / Confirm |
| **Esc** | Cancel / Exit |
| **Middle Click**| Apply Retouch (Quick Action) |
| **Right Click** | Cancel Retouch (Quick Action) |
| **[ / ]** | Adjust Brush Size |

## Installation
Requires **Python 3.10+** and `onnxruntime-gpu` for maximum performance.
C++ code builds with **Qt 6 / CMake** on Windows, Linux, and macOS.

---
*Based on the original [qView](https://github.com/jurplel/qView) by Jurplel.*
