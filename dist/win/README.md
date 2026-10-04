# Windows installer

`iqView.iss` is an [Inno Setup 6](https://jrsoftware.org/isinfo.php) script. To build the
installer locally:

1. Build the Release exe (`cmake -B build && cmake --build build --config Release`).
2. Stage a deployment in `dist/win/iqView-Win64/`: copy `build/Release/iqView.exe` there, run
   `windeployqt --no-compiler-runtime iqView.exe` on it, and copy the repo's `scripts/` folder in
   (without `scripts/.venv` and `__pycache__`).
3. From `dist/win/`, run `ISCC.exe iqView.iss`. Pass `/dMyArch=x86` or `/dMyArch=arm64` for the
   other architectures, staged in `iqView-Win32/` or `iqView-WinArm64/` instead.

The installer lands in `dist/win/Output/`. CI does steps 2–3 via `dist/scripts/windeployqt.ps1`
and `dist/scripts/innomake.ps1`, but only for versioned releases, not nightlies.
