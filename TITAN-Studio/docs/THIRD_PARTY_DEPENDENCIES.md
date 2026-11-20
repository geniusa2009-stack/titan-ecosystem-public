# Third-Party Dependencies (SBOM summary)

## TITAN 0.6 build and runtime

| Component | Version (V) | Used by | How | Licence |
|---|---|---|---|---|
| Apple clang / libc++ | 17.0.0 (Xcode 26.1.1) | all C++ | toolchain | Apple / Apache-2.0 with LLVM exception |
| CMake | 4.4.5 | build | tool | BSD-3-Clause |
| Ninja | 1.13.2 | build | tool | Apache-2.0 |
| Qt Core, Gui (+GuiPrivate for QRhi), Network, Qml, Quick, QuickControls2 | 6.12.0 (Homebrew) | TITAN Studio | dynamic link | LGPL-3.0 (or commercial) |
| Qt Shader Tools (`qsb`) | 6.12.0 | TITAN Studio build | **build-time tool only** | GPL-3.0 (or commercial). The baked `.qsb` shader output is TITAN's own content. |
| Python | 3.13 | legacy v0.5 installer tests (not run in CTest) | tool | PSF |

`titan_core`, `titan_engine`, `titan-engine-host`, the extension SDK and the scatter extension have **no
third-party dependencies** beyond the C++ standard library and POSIX. JSON, IPC, math, meshes, picking and
undo are original TITAN code.

Not used: Qt Quick 3D (GPL), nlohmann/json, any engine code from Unity, Godot, O3DE, Bevy, CRYENGINE,
Stride, Wicked or Flax.

## Unreal Engine 5.8.4 archive (inventoried, not built)

The archive carries 862 `.tps` (third-party software) notice files across 162 `Engine/Source/ThirdParty`
directories (e.g. WebRTC, ICU, FBX SDK, CEF3, FreeImage, ANGLE). These keep their individual licences. A
per-component SBOM should be generated from the `.tps` files when the engine is first built and packaged.
That is tracked in NEXT_MILESTONES.md.
