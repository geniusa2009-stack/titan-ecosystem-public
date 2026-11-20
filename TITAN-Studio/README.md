# TITAN Engine 0.6.0-alpha: Integrated Vertical Slice

A native editor (**TITAN Studio**, Qt 6 / QML) connected over a versioned local protocol to an engine
process that owns the canonical project and scene. Includes a real GPU viewport, inspector editing,
undo/redo, persistence, a native extension SDK, and a deterministic World Builder operator with
preview → approve.

```bash
brew install cmake ninja qtbase qtdeclarative qtshadertools
cmake -S . -B build -G Ninja && cmake --build build -j 4
ctest --test-dir build --output-on-failure
open "build/bin/TITAN Studio.app"
```

## What works (verified on Apple M3 / macOS 26.5.1, 11/11 test suites)
- Studio launches the engine host, negotiates protocol v1, and creates or opens projects (with a recent
  projects list).
- World workspace: hierarchy, Metal viewport (orbit/pan/zoom, click-to-select via engine picking,
  ⌥-drag move), searchable Inspector (name, transform, colour, visibility, light intensity), undo/redo,
  save, and reopen with the same state, camera and selection.
- Light and dark themes (Apple Pro Hybrid tokens), command palette (⌘K), text scaling, reduced motion,
  English/Arabic toggle with RTL mirroring (partial translation).
- World Builder: `titan.procedural.scatter` extension → ghost preview → approve as one undo step →
  recipe saved → selective regeneration.
- Performance workspace with measured viewport, IPC and engine metrics; Extensions workspace.

## What does not work yet
The engine fork base is **Unreal Engine 5.8.4** (owner decision D-1). Its working copy and baseline branch exist
in `../engine/UE-5.8.4`, but it is **not built yet**: it needs the ~21 GB dependency download (macOS only).
The Unreal bridge plugin is written but **not compiled**. The Engineering Agent is specified but not
implemented. Details: `docs/KNOWN_LIMITATIONS.md`, `docs/TEST_REPORT.md`.

## Layout
`core/` shared contracts · `engine/host/` engine process · `sdk/` extension ABI · `extensions/` ·
`studio/` editor · `tools/protocol-check/` conformance tool · `tests/` · `unreal/Plugins/TitanStudioBridge/` ·
`legacy/TitanFoundation-v0.5/` (unmodified prior TITAN code) · `docs/`.

Start with `docs/ARCHITECTURE.md`, then `docs/EDITOR_INTEGRATION.md` and `docs/SOURCE_INVENTORY.md`.
