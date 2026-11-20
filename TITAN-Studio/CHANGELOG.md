# Changelog

## 0.6.1-alpha (in progress, 2026-11-06): Unreal Engine 5.8.4 integration path
- **First full TitanHostEditor build: exit 0** (1,609 actions, 4,038 s, `--jobs 2`, 8 GB M3, log
  `20261109T121208Z-build.log`): arm64 `UnrealEditor` and `libUnrealEditor-TitanStudioBridge.dylib` linked.
- `scripts/sync_ue_plugins.sh`: copies by content without preserving modification times. `rsync -a` gave an
  edited file its older repo mtime, so UBT reported "Target is up to date" and kept a stale bridge
  (log `20261109T132033Z-build.log`). New `tools.preflight` cases (`PluginSync`) fail with `rsync -a`.
- Frame channel seqlock: the bridge writer now fences (release) after marking a slot odd, and the Studio
  reader fences (acquire) before re-reading `seq`; the previous release-store / acquire-load pair did not
  order the pixel copy on ARM64. New concurrent writer/reader test (`studio.frame-channel`) catches a
  reader that skips the second `seq` check; it does not reliably expose the missing fences on an M3.
- Frame channel reader hardening: slot geometry (`header_bytes`, `slot_bytes`) is validated once at attach
  (overflow-free) and cached; a changed or corrupt header can no longer make the reader read outside the
  mapping (mutation: re-reading it from shared memory crashes the test with SIGBUS). Deterministic tests
  for the second `seq` check (test hook between copy and re-check) and for overflowing geometry. The
  full ordering contract, slot reuse and the language-level data-race note are in `TitanFrameChannel.h`.
- Engine fork moved to an ASCII path (`~/TitanEngine/UE-5.8.4`, symlinked from `engine/UE-5.8.4`); APFS clone
  sharing measured (0.47 GB private of 3.34 GB).
- `tools/ue_build_preflight.py`: measured disk and dependency budget; refuses to start without 35 GB headroom.
- `scripts/ue_build.sh`: macOS-only GitDependencies (`--no-cache`, 105 reviewed exclusions), `bridge-check`
  (single-file compile of the TITAN sources), project-scoped `TitanHostEditor` build, full logs, disk guard.
- `unreal/TitanHost`: host project (`DisableEnginePluginsByDefault`, Nanite/Lumen/VSM settings, SM6 only).
- TitanStudioBridge v2: protocol-v1 parity with the reference host on real Unreal levels and actors, exact
  rotation conversion, coalesced-gesture transactions, echo of Unreal-side edits, line-trace picking,
  Unreal-rendered viewport frames (scene capture → async readback → shared memory), renderer statistics,
  engine automation tests. **Written; not compiled yet.**
- Studio: protocol I/O on a dedicated thread. In the smoke test the average round trip fell from
  12.9–15.7 ms to 0.22–0.30 ms. Unreal backend (launch, engine selector, command-palette entries),
  engine-rendered viewport frames with Studio overlays, Unreal metrics in the Performance workspace.
- Tests: `ue.coordinates`, `studio.frame-channel` (new, passing); `titan-protocol-check --viewport`;
  `ue.bridge-conformance` and `ue.automation` (skip until the editor is built). 15 suites.
- Preflight per stage (`deps`, `plan`, `bridge-check`, `build`) and per storage pool (engine, DDC/Zen, temp,
  swap; APFS volumes pooled by container); the guard watches every affected pool; the 35 GB floor cannot be
  lowered. Guard race fixed (stop signal could miss the step's process group). `deps`: 4 threads and a
  bounded HTTP timeout after a multi-hour stall; .NET steps run as `en_US.UTF-8` (under `ar_EG`
  GitDependencies rejected a decimal argument). New suite `tools.preflight`: 16 suites.
- Preflight review fixes (PR #2): nothing already on disk (downloaded dependencies, git-tracked or
  untracked files, `Engine/DerivedDataCache`) reduces the build-output estimate any more; the DDC is budgeted where UE 5.8.4 actually writes it
  (`~/Library/Application Support/Epic/UnrealEngine/Common/Zen/Data` by default, following the engine's
  override order) plus its Local file-system fallback; the preflight-only `TITAN_UE_DDC` override was
  removed. `scripts/ue_build.sh` runs one Unreal step at a time (exit 4 otherwise), using a kernel
  `flock` with no stale-lock reclaim (review on PR #2). `tools.preflight` now has 34 cases.
- `deps`: HTTP timeout multiplier 0.25 → 0.75 (~60 min for the largest pack): three attempts at the last three
  ~300 MB packs timed out at 150–400 KB/s before the fourth completed. Dependencies complete (exit 0); all
  98,381 selected files match their SHA-1 hashes.
- First real compile of the TITAN Unreal modules against UE 5.8.4 (`bridge-check`, 1 job): UBT plan passes;
  two UE 5.8 API errors fixed in `TitanBridgeSession.cpp` (`Policies/CondensedJsonPrintPolicy.h` include;
  `FJsonObject` keys are `UE::FSharedString`); all six TITAN `.cpp` files compile with 0 errors, 0 warnings.
  Not linked yet. Both facts added to `ue.source-contract`.
- CI: `.github/workflows/titan-ci.yml` (core built for macOS 13.3 and run on macOS 15; Qt Studio and all suites
  on macOS 26; per-platform 15-suite report; Xcode 26.1.1 pinned and verified) and the hand-started,
  self-hosted `titan-unreal.yml` for the real-engine suites; helpers in `tools/ci/`.
- Core: JSON number parsing no longer requires macOS 26. `std::from_chars(double)` is used where the deployment
  target allows it, otherwise a `strtod_l("C")` fallback with the same results (5 new tests). Core, SDK and
  Engine Host build for macOS 13.3.
- `tools/ue-bench`: renderer A/B benchmark (Nanite, Lumen, VSM, RDG, GPU Scene, memory) for the real engine.

## 0.6.0-alpha (2026-11-05): Integrated Vertical Slice
- New `titan_core`: JSON, math, scene model with stable ids, reversible edits and undo/redo with
  coalescing and dirty tracking, atomic project persistence, exact primitive picking, meshes, protocol
  framing, POSIX IPC.
- New `titan-engine-host`: authoritative engine process serving the titan.editor v1 protocol, with
  metrics and event broadcast.
- New TITAN Extensions SDK (C ABI v1, manifest validation, dependency resolution) and the
  `titan.procedural.scatter` World Builder extension.
- New TITAN Studio (Qt 6.12 / QML): launcher, World/Performance/Extensions workspaces, QRhi Metal
  viewport, Inspector, hierarchy, World Builder preview/approve, command palette, light/dark, EN/AR.
- New `titan-protocol-check` conformance tool. 9 CTest suites including a GUI smoke test.
- New `TitanStudioBridge` UE 5.8.4 editor plugin (source only, not compiled).
- Imported TITAN v0.5 unchanged; its tests run in CTest.
- Documentation set under `docs/`.
