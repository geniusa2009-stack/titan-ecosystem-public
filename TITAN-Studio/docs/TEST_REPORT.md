# Test Report: TITAN 0.6.1-alpha

## 2026-11-09: first real Unreal Engine 5.8.4 integration

Same machine (Apple M3, 8 GB, macOS 26.5.1, Xcode 26.1.1). Logs in `~/TitanEngine/logs` and
`~/TitanEngine/sessions` (local, not committed). Screenshots and one report are in `docs/screenshots/unreal/`.

- **Full `TitanHostEditor` build: exit 0.** 1,609 UBT actions in 4,038 s (`--jobs 2`), 0 compile errors, 2 linker
  warnings (`ignoring duplicate libraries: '-lc++'`). Log `20261109T121208Z-build.log`. Outputs (arm64):
  `Engine/Binaries/Mac/UnrealEditor.app` and 475 `libUnrealEditor-*.dylib`, plus
  `Engine/Plugins/TITAN/TitanStudioBridge/Binaries/Mac/libUnrealEditor-TitanStudioBridge.dylib`. The bridge's
  9 engine-library dependencies all resolve, and its module manifest has the same BuildId as `UnrealEditor.version`.
  `ShaderCompileWorker` is a separate target; it is now built by the same step (209 actions, exit 0).
- **Runtime prerequisites found by running the editor:**
  - **Metal Toolchain.** Xcode 26 ships it separately. Installed with owner approval through
    `xcodebuild -downloadComponent metalToolchain` (17B54, 704.6 MB).
  - **ShaderCompileWorker.** Missing before the change above.
  - Without either one, UnrealEditor quits at startup.
- **`ctest --test-dir build -L unreal` → 2/2 passed:**
  - `ue.bridge-conformance`: 18/18 steps against the real UnrealEditor + TitanStudioBridge, including Unreal-rendered
    frames, a camera change, an object edit visible in the frame, and the new 4-size resize step.
  - `ue.automation`: 2/2 (`TITAN.Bridge.CoordinatesMatchUnreal`, `TITAN.Bridge.FrameChannelLayout`).
  - The first launch compiled all shaders in 477 s; later launches are ready in about 12 s.
- **Real TITAN Studio ↔ Unreal sessions** (`TITAN Studio --smoke-test … --engine unreal`, 32 steps):
  - **Before the readback fix:** 1 run passed (`20261109T142005Z`). Three runs then crashed Unreal (render-thread
    SIGSEGV, defect 1 below).
  - **After all fixes, 3 timed runs:**
    - `…145318Z-r2` and `…145507Z-r3` passed, with **29 PASS, 3 SKIP, 0 FAIL** each.
    - `…144922Z-r1` **FAILED**: 60 sequential `engine.status` calls during continuous rendering did not finish in
      120 s. Unreal ticked about once per second at 43 % average CPU, swap rose to 5.4 GB and free memory fell to
      5 %. No crash. Root cause not established; memory starvation is the leading hypothesis.
  - **Steps that passed:**
    - Protocol negotiation and project creation through Unreal.
    - Engine-rendered frames on screen.
    - Click-to-pick selection and the Inspector.
    - Typed number input.
    - Inspector edit and API edit.
    - Drag-orbit with new engine frames.
    - Create and undo of a cube; undo and redo.
    - Save, close and reopen with the edit persisted.
    - Disconnect and reconnect, which relaunches Unreal and recovers the project.
  - **SKIP (not passed):** the 3 World Builder steps, because the bridge does not advertise `worldbuilder.preview`.
- **Measured** (3 passing sessions; mean, with the range in brackets):

  | Measure | Result |
  |---|---|
  | Engine connect | 10.9 s [8.3–12.5] |
  | Engine frames per second (measured interval) | 8.0 [6.8–9.3] |
  | Engine GPU per frame | 217 ms [73–322] |
  | Capture → shown in Studio | 670 ms [229–1099] |
  | Round trip while rendering | avg 252 ms, max 1.58 s |
  | Pick | 1.47 s |
  | Inspector edit | 0.68 s |
  | Undo / redo | 0.49 / 0.54 s |
  | Save | 1.47 s |
  | Reconnect | 20.5 s |
  | UnrealEditor peak RSS | 0.87–1.64 GB |
  | Swap during a session | grows to about 5 GB |

  **60 fps is not achieved:** Unreal renders this 802×828 view at single-digit frame rates on this machine.
- **Defects found by real runs and fixed** (each with a regression check that fails without the fix):
  1. Render-thread SIGSEGV after a viewport resize. Pooled `FRHIGPUTextureReadback` objects kept their
     old staging size. Reproduced by the new conformance step, and fixed in `TitanBridgeFrames.cpp`.
  2. Every Inspector number field refused typed input. The PCRE2-invalid `\u200E` in the validator regex made the
     whole validator invalid; a new smoke step types into the field.
  3. A stale bridge after sync. `rsync -a` kept the old mtime, so UBT saw nothing to rebuild; covered by the
     `PluginSync` tests.
  4. The build did not produce `ShaderCompileWorker`; covered by the `BuildStepTargets` test.
  5. The viewport HUD showed Studio's 60 Hz re-presentation instead of the engine's frame rate.
  6. The Performance workspace logged 11 QML TypeErrors at startup.
  7. Frame-channel seqlock fences, plus overflow-safe cached geometry (also PR #3).

# Earlier results (0.6.0-alpha)

Machine: Apple M3, 8 GB unified memory, macOS 26.5.1 (Tahoe), arm64. Toolchain: Apple clang 17.0.0
(Xcode 26.1.1), CMake 4.4.5, Ninja 1.13.2, Qt 6.12.0 (Homebrew). Date: 2026-11-05.

Command: `ctest --test-dir build --output-on-failure` → **14 passed, 2 skipped (not run), 0 failed of 16**
(2026-11-06, 0.6.1). The two skipped suites need the Unreal Editor, which is not built yet. CTest reports them
as *Skipped*, never as passed.
A fresh clean build in a separate directory reproduced 8/8 non-GUI suites (`ctest -LE gui`).

**First real Unreal compile of the TITAN modules (2026-11-06, `scripts/ue_build.sh bridge-check --jobs 1`).**
UBT `plan` (log `20261106T202416Z-plan.log`, exit 0): UBT built itself (0 warnings, 0 errors), created the
`TitanHostEditor` makefile, and UHT processed the target in 11.3 s (9,790 generated files). `bridge-check`
compiles the TITAN sources with UBT's real `TitanHostEditor` compile environment (clang from Xcode 26.1.1,
macOS 26.1 SDK, `-std=c++20`, `arm64-apple-macos14.00`, `WITH_EDITOR=1`, `UE_BUILD_DEVELOPMENT=1`,
`WITH_DEV_AUTOMATION_TESTS=1`). The first run (log `20261106T202919Z-bridge-check.log`, exit 6) failed in
`TitanBridgeSession.cpp` with two real UE 5.8.4 API errors that the static contract check could not see:
`TCondensedJsonPrintPolicy` undeclared (its header is no longer included by `JsonSerializer.h`) and
`FJsonObject::Values` keyed by `UE::FSharedString`, not `FString`. After the fix (log
`20261106T203116Z-bridge-check.log`, exit 0, 0 errors, 0 warnings) all six compile to arm64 objects:

| File | Object |
|---|---|
| `TitanStudioBridge/Private/TitanBridgeSession.cpp` | 473,136 B |
| `TitanStudioBridge/Private/TitanBridgeFrames.cpp` | 105,040 B |
| `TitanStudioBridge/Private/TitanStudioBridgeModule.cpp` | 43,848 B |
| `TitanStudioBridge/Private/Tests/TitanBridgeTests.cpp` | 19,528 B (automation tests compiled in) |
| `TitanStudioBridge/Private/TitanBridgeTransport.cpp` | 15,824 B |
| `TitanHost/TitanHost.cpp` | 3,416 B |

This is compilation only. The plugin is **not linked** (that needs the engine modules from the full editor
build, which is still blocked by disk space) and nothing has run inside Unreal yet.

| Suite | Kind | Cases | Result | What it proves |
|---|---|---|---|---|
| `core.unit` | unit | 23 | PASS | JSON (unicode/Arabic, malformed input, nesting limit; numbers: exact results at the edges of double, invalid text rejected, `strtod_l` fallback vs `std::from_chars` bit-for-bit on 300k inputs where both exist, 200k-value bit-exact round trip, decimal-comma locale), matrices/inverse, exact primitive picking, hierarchy picking, mesh validity, scene round-trip + validation (version, duplicate ids, orphan parents, cycles), undo/redo of edits and deletes, coalescing rules, dirty tracking incl. unreachable save point, atomic project save/reload in a non-ASCII path, manifest path escape rejected, fragmented/oversized frames |
| `extensions.unit` | unit | 5 | PASS | Manifest validation (API version, id, semver, platform, capabilities, category, library path traversal, engine range), dependency failure, undeclared command refusal, real `dlopen` of the scatter extension, determinism (same seed → identical, different seed → different), spacing/keep-out constraints, bad input |
| `host.integration` | integration (real process) | 4 | PASS | Spawns `titan-engine-host`; negotiation required; version mismatch rejected; two clients share selection events; update → pick at new location → undo/redo → validation errors → unsaved-close refused → save → close → reopen with stable ids and restored selection; World Builder preview determinism, preview doesn't mutate scene, stale proposal refused, approve = one undo step, selective regeneration keeps group, recipe persisted; malformed and oversized frames plus abrupt disconnects don't take the host down |
| `host.protocol-conformance` | conformance | 12 steps | PASS | `titan-protocol-check` against the host (reusable for the Unreal bridge) |
| `ue.source-contract` | static pre-flight | 26 signatures + 63 includes + 9 modules + Xcode range | PASS | The supplied UE 5.8.4 tree provides every include, module and API signature the TITAN Unreal plugins use, and local Xcode 26.1.1 = UE's main supported version. Negative-tested (bad Xcode, missing include/module → fail). Not a compile. |
| `ue.plugin-sync` | consistency | 1 plugin | PASS | The engine fork's `Engine/Plugins/TITAN` copy matches `unreal/Plugins` (drift detection negative-tested) |
| `ue.coordinates` | unit | 7 | PASS | The bridge's own `TitanCoordinates.h` against TITAN's `Transform::matrix`: Euler round trips incl. gimbal lock, UE and TITAN rotations move the same physical vector (2000 random cases, < 1e-9), yaw handedness, axis table, camera/FOV conversion, frame-channel layout |
| `studio.frame-channel` | unit | 11 | PASS | Studio's `RemoteFrameSource` against a writer that follows the bridge's seqlock protocol: bad layout rejected, newest complete frame served once, in-progress frame never served, overwritten slot detected, size change and re-attach, repaint hook |
| `tools.preflight` | unit + script | 38 | PASS | Per-stage, per-pool disk budget of `tools/ue_build_preflight.py` (APFS container pooling, independent external pools, unaffected pools ignored, `deps` no longer budgeted as a full build, 35 GB floor cannot be lowered, simulation can only lower free space), CLI on a fake engine (dependency bytes from the manifest, simulated low-space FAIL), and `scripts/ue_build.sh` refusing to start on a simulated low-space preflight and its guard stopping a running step at the floor. Build-output credit: a large downloaded file in `Engine/Binaries/Mac`, git-tracked files and `Engine/DerivedDataCache` do not reduce the build budget, and untracked files are reported but not deducted either; a second step is refused while one is running; run lock: held lock refused, two concurrent contenders (fresh and with a stale lock file plus an old lock directory) never both run, a SIGKILLed holder releases the lock (mutation-checked: no lock lets both run; the old mkdir/rm reclaim fails the stale-lock case). DDC location follows UE 5.8.4's resolution order (default, `UE-ZenDataPath`, `UE-LocalDataCachePath` in and outside the engine, KeyValueStore precedence) and budgets a fallback on another pool. Mutation-tested: restoring the old deduction, disabling the guard, the floor checks, pool summing or the `deps` budget each fail it. The two shell cases that need a passing preflight skip on hosts with < 40 GB free. |
| `ue.bridge-conformance` (label `unreal`) | integration, real Unreal | 18 steps | **PASS (2026-11-09)** | Launches UnrealEditor + TitanStudioBridge and runs `titan-protocol-check --viewport` (incl. resize step). |
| `ue.automation` (label `unreal`) | engine automation | 2 | **PASS (2026-11-09)** | `TITAN.Bridge.*` inside Unreal: coordinate math equals `FRotationMatrix`; frame layout. |
| `titan_unit`, `titan_legacy_planner`, `titan_data`, `titan_telemetry` | unit (TITAN v0.5, unmodified) | — | PASS | Prior TITAN code still builds and passes under C++14 inside the 0.6 tree |
| `studio.ui-smoke` (label `gui`) | UI end-to-end | 24 steps | PASS | Real Studio window + real engine process + Metal viewport: connect → create project → frames rendered → **synthetic click in viewport selects Hero Cube via engine pick** → Inspector shows it → **edit X through the Inspector field** → edit Z via API → 60 fps continuous render, light screenshot → dark theme screenshot → undo/redo → World Builder preview ghosts → approve → save → close → reopen restores transform + recipe → screenshot |

Smoke stability: after fixing a test-condition bug (see below), passed **3/3 consecutive runs** and again under CTest.
0.6.1 (I/O thread): 3/3 consecutive runs plus later CTest runs. New steps: launcher screenshot, 60 engine
round trips during continuous rendering (avg 0.023–0.027 ms), recorded in `smoke-report.json`.

Screenshots from the last run: `build/smoke/world-light.png`, `world-dark.png`, `world-preview.png`,
`world-reopened.png` (2880×1800, Retina). JSON report: `build/smoke/smoke-report.json`.

## Defects found and fixed during verification
1. Conformance checker stopped waiting after an unrelated earlier event (test bug, fixed).
2. QML: `onAccent` token parsed as a signal handler (fixed: `textOnAccent`).
3. Repeater-created Inspector fields not reachable through the `QObject` tree (fixed: test walks the item tree).
4. Transparent ghost previews showed back faces and cap seams (fixed: back-face culling on the ghost pipeline; verified in screenshot).
5. Viewport stats lagged by one frame and could stall in on-demand mode (fixed: lock-free stats published after `frameSwapped`, plus repaint when a workspace becomes visible).
6. **RTL bug:** negative numbers rendered as "1.5-" when the macOS input source is Arabic (fixed: numeric fields pinned LTR with U+200E; verified in screenshot).
7. Stats chip showed "On-demand" while live rendering was on (fixed: binds to actual viewport state).

## NOT RUN
| Item | Reason |
|---|---|
| World Builder against Unreal | The bridge does not implement `worldbuilder.*`; the smoke test reports these steps as SKIP. |
| Stable Studio ↔ Unreal session under memory pressure | 1 of 3 timed runs failed (see above); the root cause is not established. |
| 60 fps Unreal viewport | Not achieved; about 4–9 engine fps measured on the 8 GB M3. |
| TITAN v0.5 UE plugin compile (`legacy/TitanFoundation-v0.5`) | Not part of the `TitanHostEditor` target; not compiled. |
| Gated GitHub workflow `titan-unreal.yml` | Needs a self-hosted Mac runner holding the engine; none registered. The hosted `titan-ci.yml` runs pass. |
| Windows, Linux builds/tests | No such machines in this session. |
| Manual exploratory UI pass (keyboard-only navigation, VoiceOver, every command) | Not performed. |
| Arabic/RTL full-UI screenshot review | The toggle exists and RTL mirroring is wired; only partially translated and not visually reviewed. |
