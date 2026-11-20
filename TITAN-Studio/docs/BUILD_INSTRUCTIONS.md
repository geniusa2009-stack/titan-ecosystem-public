# Build Instructions

## TITAN (core, engine host, extension, tools, Studio, tests): macOS (verified)

Prerequisites (verified versions): Xcode 26.1.1 command-line tools, Homebrew.

```bash
brew install cmake ninja qtbase qtdeclarative qtshadertools
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j 4
ctest --test-dir build --output-on-failure            # all 14 suites (opens a window for studio.ui-smoke)
ctest --test-dir build -LE gui --output-on-failure    # headless suites only
```

Run the editor:

```bash
open "build/bin/TITAN Studio.app"
"build/bin/TITAN Studio.app/Contents/MacOS/TITAN Studio" --open /path/to/Project
```

Options: `-DTITAN_BUILD_STUDIO=OFF` (no Qt needed), `-DTITAN_BUILD_LEGACY=OFF`, `-DTITAN_BUILD_TESTS=OFF`.
Studio finds `titan-engine-host` next to its executable or inside the bundle first, then falls back to the
build-tree path. `--engine-host <path>` overrides this.

Engine host standalone: `build/bin/titan-engine-host --socket /tmp/t.sock --extensions build/extensions [--project dir]`.
Protocol check: `build/bin/titan-protocol-check --socket /tmp/t.sock --workdir /tmp/conf --shutdown`.

## macOS versions: deployment vs. build environment

These are separate questions.

| Component | Minimum macOS it can be built for | Runtime verified on | Notes |
|---|---|---|---|
| TITAN Core, SDK, Engine Host, extensions, tools | **13.3** (`-DCMAKE_OSX_DEPLOYMENT_TARGET=13.3`) | macOS 15 (CI), macOS 26 (CI, reference Mac) | 13.3 is set by `std::to_chars(double)`. Below macOS 26.0 JSON numbers are parsed with the `strtod_l("C")` fallback (`core/include/titan/detail/json_number.h`), verified bit-for-bit against `std::from_chars`. macOS 13.3–14.x is compile-verified only. |
| TITAN Studio | TITAN code compiles for 13.3; the effective floor is also bounded by the Qt build used (the CI summary prints the QtCore minimum) | macOS 26 only (CI, reference Mac) | Not yet built or run against an older deployment target in CI. |
| Unreal Engine 5.8.4 fork (build environment) | n/a | — | Needs a Mac that runs **Xcode 26.1.1** (`Engine/Config/Apple/Apple_SDK.json`: MainVersion 26.1.1, range 15.2–27.9). That is the toolchain for building the engine, not a deployment floor for Studio. |

With no `CMAKE_OSX_DEPLOYMENT_TARGET`, CMake builds for the host's macOS version; on macOS 26 that selects
`std::from_chars`, so the reference build is unchanged.

## Continuous integration (GitHub Actions)

`.github/workflows/titan-ci.yml` runs on pull requests to and pushes on `titan/integration` (docs-only
changes are skipped). Both macOS jobs select **Xcode 26.1.1** explicitly (`DEVELOPER_DIR`) and fail if a
different Xcode is active; GitHub's images default to newer Xcode versions.

| Job | Runner | What it does |
|---|---|---|
| Core, SDK, Engine Host | `macos-15` | builds everything except Studio for deployment target 13.3, checks the binaries' `minos`, runs every registered suite on a real macOS 15 (exercises the `strtod_l` JSON path) |
| TITAN Studio and all suites | `macos-26` | Qt 6.12.0 from download.qt.io (aqtinstall, cached); full build; `qmllint` report (only critical findings fail); linked-framework licence check (no Qt Quick 3D / Shader Tools at runtime); Metal probe; every registered suite, including the GUI smoke test when a Metal device exists (exercises the `std::from_chars` path and the differential test) |
| Test report | `ubuntu-latest` | `tools/ci/ctest_report.py` accounts for each of the 15 baseline suites per platform: passed, failed, skipped, not available because the Unreal source is absent, needs a self-hosted Mac, plus QML lint counts. Fails if a suite fails or disappears |

The private Unreal source is never downloaded, cached or uploaded by standard CI, so `ue.source-contract`
and `ue.plugin-sync` are NOT RUN and `ue.bridge-conformance` / `ue.automation` are SKIPPED. None of these
counts as integration evidence. They run in `.github/workflows/titan-unreal.yml`, which can only be started by
hand and only on a self-hosted Mac with the label `titan-unreal` that holds the engine locally
(`TITAN_UE_WORKING_COPY`, `TITAN_UE_SOURCE_ROOT` in the runner's environment); it also pins Xcode 26.1.1 and
checks it against the engine's `Apple_SDK.json`. In that workflow a skipped Unreal suite is a failure. No such
runner is registered yet.

Workflow permissions are `contents: read`; every action is pinned to a commit SHA. On a private
repository GitHub bills macOS runner minutes at a multiple of Linux minutes, so CI cancels superseded
runs and starts the Qt job only after the core job passes.

## Linux / Windows
The CMake targets are portable C++20 + Qt. The IPC (`ipc_posix.cpp`) and extension loading (`dlopen`)
are POSIX; Linux should work but is **unverified**. Windows needs a named-pipe transport and
`LoadLibrary`. Both are explicitly `#error`'d today.

## Unreal Engine 5.8.4 (fork base, decision D-1)

Layout (decision D-8, ASCII-only paths for Unreal's toolchain):

| Path | What |
|---|---|
| `~/TitanEngine/UE-5.8.4` | engine fork, git `upstream/ue-5.8.4-baseline` (unmodified) → `titan/engine` (TITAN plugins) |
| `محرك/engine/UE-5.8.4` | symlink to the fork (keeps the original layout and CMake default) |
| `~/TitanEngine/TitanHost` | TITAN host project, synced from `unreal/TitanHost` |
| `~/TitanEngine/logs` | full, unfiltered logs, `*.metrics.csv` disk samples, preflight JSON |

The fork is an APFS clone of the read-only vault tree: measured APFS-private bytes 0.47 GB of 3.34 GB
(`tools/ue_build_preflight.py --apfs-tree`, which sums `ATTR_CMNEXT_PRIVATESIZE`), so it does not duplicate the engine.

### 1. Preflight per stage and per storage pool (35 GB headroom on every pool a step writes to)

```bash
python3 -I tools/ue_build_preflight.py --engine ~/TitanEngine/UE-5.8.4 \
  --exclude-file unreal/BuildConfig/gitdeps-macos.exclude --stage deps   # or plan | bridge-check | build
```

Each `scripts/ue_build.sh` step runs the preflight for its own stage and refuses to start on FAIL. The
preflight measures the dependency bytes still missing and the free space of every storage *pool* the stage
writes to: the engine tree, the derived data cache, the per-user temporary directory and the swap volume.

The derived data cache is budgeted where Unreal itself will write it. The preflight follows UE 5.8.4's own
resolution order (`Engine/Source/Developer/Zen/Private/ZenServerInterface.cpp`, `DetermineDataPath`):
`UE-ZenSubprocessDataPath`, then `[Zen] DataPath` in `~/Library/Application Support/Epic/Epic Games/KeyValueStore.ini`,
then `UE-ZenDataPath`, then `UE-LocalDataCachePath` + `/Zen` when it is outside the engine tree, else the
`[Zen.AutoLaunch] DataPath` default, which on macOS is
**`~/Library/Application Support/Epic/UnrealEngine/Common/Zen/Data`** (outside the engine tree, on the
internal disk even when the engine lives elsewhere). It also checks the Local file-system DDC that the
default cache graph falls back to when Zen is unavailable (`Engine/DerivedDataCache`, or
`UE-LocalDataCachePath`); if that is on another pool, it must hold the whole DDC estimate too. On this Mac
none of the overrides is set (checked 2026-11-06), so both are on `disk3`. There is no TITAN-only DDC
override: `--ddc-path` exists for tests and is reported as "preflight only; Unreal does not use it". To move
the cache, set Unreal's own `UE-ZenDataPath`/`UE-LocalDataCachePath` so the engine moves it too.

The build-output estimate is never reduced by what already exists in `Engine/Intermediate` and
`Engine/Binaries/Mac`: those folders hold downloaded dependencies, git-tracked files and leftovers of other
targets, and even an untracked file is no proof that this build produced it. The preflight reports what is
present (and how much of it is untracked and not downloaded) but deducts nothing; over-reserving after an
earlier partial build is the safe direction. (Before this fix the 0.5 GB of downloaded binaries in those
folders was deducted from the 25 GB estimate.)

Only one Unreal step runs at a time: `scripts/ue_build.sh` refuses (exit 4) while another run holds the
exclusive `flock(2)` on `$TITAN_UE_LOGS/ue_build.flock`, while a guarded step is still running (a live
`<log>.pid`, also written by older copies of the script), or while any GitDependencies/UBT process of the
same engine is alive. The kernel takes the lock atomically and drops it when its holder exits or is killed,
so there is no stale lock to reclaim (the earlier `mkdir`/`rm` reclaim could race); the lock file is never
removed, and steps run with the lock's descriptor closed so no lingering child keeps it. APFS volumes in one
container (Data, VM) share free space, so their needs are added up; a separate disk is a separate pool with its
own 35 GB floor. The floor cannot be lowered (`--headroom-gb`/`--floor-gb` below 35 are refused).

| Stage | Budgeted on top of the missing dependencies | Basis |
|---|---|---|
| `deps` | temp 1 GB | `--no-cache` writes files in place |
| `plan` | UBT/UHT binaries + rules assemblies 1 GB, temp 1 GB | **estimate** |
| `bridge-check` | UHT output + PCHs + TITAN objects 6 GB, temp 1 GB, swap 2 GB | **estimate** |
| `build` | editor output 25 GB (nothing deducted), DDC 10 GB, temp 3 GB, swap growth 8 GB | **estimates** |

Measured on 2026-11-06 17:39 UTC (M3, 8 GB RAM, internal APFS container `disk3`, 76.0–76.1 GB free; swap
17.2 GB used of 18.3 GB, 30 % memory free):

| Stage | Needs on `disk3` | Projected free | Result |
|---|---|---|---|
| `deps` | 15.8 GB | 60.3 GB | PASS |
| `plan` | 16.5 GB | 59.5 GB | PASS |
| `bridge-check` | 23.5 GB | 52.5 GB | PASS |
| `build` | 60.5 GB | 15.5 GB | **FAIL**, short by 19.5 GB |

Earlier measurements: 113.0 GB free at the first preflight; a Unity install (`/Applications/Unity`, 17 GB)
and swap growth (swap files 13 → 18 GB) since then are why the full build no longer fits.

**External storage checked 2026-11-06:** the only external device is a 31.5 GB USB 2.0 flash drive
(480 Mb/s link, MBR, NTFS mounted read-only by macOS, 9.7 GB free, holds personal files). It is not usable
for the engine, build output or DDC (read-only, too small, too slow, no APFS semantics) and was not modified.
A full editor build here needs either ~20 GB more free on the internal disk, or an APFS-formatted external
SSD (USB 3.2/Thunderbolt, ≥ 100 GB free) for the derived data and/or the engine fork.

The estimates cannot be measured before the build exists. `scripts/ue_build.sh` enforces the real limit: it
samples every affected pool every 15 s and stops the step (SIGTERM, then SIGKILL) if any falls below 35 GB.
`tests/test_ue_build_preflight.py` (CTest `tools.preflight`) covers the budget logic, a simulated low-space
refusal and the guard stopping a running step.

Exclusions (`unreal/BuildConfig/gitdeps-macos.exclude`), with semantics checked against
`GitDependencies/Program.cs` (`IsExcludedFolder`): other platforms and their toolchains (Win*, Linux*, Unix,
Android, IOS, TVOS, VisionOS, VS2015/vs2017, linux/win runtime folders), optional MetaHuman sample content, and
Unreal editor UI translations other than English and Arabic. All platforms would be 32.6 GB download / 91.4 GB
unpacked; macOS-only without the content exclusions would be 24.5 GB / 60.4 GB.

**Important:** GitDependencies keeps a download cache in `.git/ue-gitdeps` (up to 2× the download) whenever
the tree has a `.git` directory, which the fork does. The script passes `--no-cache`.

### 2. Dependencies and build (monitored)

```bash
scripts/sync_ue_plugins.sh                   # plugin -> Engine/Plugins/TITAN, project -> ~/TitanEngine/TitanHost
scripts/ue_build.sh deps                     # macOS-only GitDependencies, --no-cache, 4 threads, disk guard
scripts/ue_build.sh plan                     # build UBT, evaluate target/module/plugin rules only (-SkipBuild)
scripts/ue_build.sh bridge-check             # compile only the TITAN sources (+ the PCHs they need)
scripts/ue_build.sh build --jobs 3           # TitanHostEditor Mac Development -NoDebugInfo -MaxParallelActions=3
scripts/ue_build.sh status
```

Reviewed before use: `Setup.sh` (would also rewrite `.git/hooks`, so it is not used), `Mac/GitDependencies.sh`
(removes quarantine from its own binary; the script does the same), `FixDependencyFiles.sh` (no-op on
case-insensitive APFS apart from copying a Services app into `~/Library`; not needed), `Mac/Build.sh`
(maps UBT exit codes 2/254/255 to success, so `RunUBT.sh` is used instead to keep the real exit code).

`deps` uses 4 threads and `--http-timeout-multiplier=0.75`: GitDependencies' per-request timeout is
(largest pack / 250 kB/s) × multiplier × threads, and each request must deliver the whole pack within it. With
12 threads and the 303 MB pack that is ~4 h, and a stalled connection sat at 0.00 MiB/s for hours (log
`20261105T231605Z-deps.log`). The next setting, 4 × 0.25 (~20 min), was too short on this link: the last three
~300 MB packs shared 150–400 KB/s, three attempts timed out and the fourth (of five allowed) completed when the
link sped up (log `20261106T173840Z-deps.log`, exit 0 at 20:22 UTC). 4 × 0.75 gives ~60 min for the largest
pack. After that run all 98,381 selected files (33.5 GB) matched their manifest SHA-1 hashes. All Unreal .NET steps run as
`en_US.UTF-8`: .NET parses numbers with the current culture, and under this Mac's `ar_EG` locale
GitDependencies rejects `--http-timeout-multiplier=0.25` as an invalid parameter (measured). Restarting `deps`
keeps everything already extracted: GitDependencies re-hashes existing files and downloads only what is
missing or different.

Build scope (decision D-9): `TitanHostEditor` is UnrealEditor built for `unreal/TitanHost/TitanHost.uproject`,
which sets `DisableEnginePluginsByDefault` and enables only `TitanStudioBridge`. That compiles the engine and
editor modules but not the ~hundreds of default-on engine plugins, which cuts build time, disk and RAM on 8 GB.
`-NoDebugInfo` keeps object files small; use `-DebugInfo=Full` on a larger machine for debugging.

### 3. Run and test against real Unreal

```bash
ctest --test-dir build -L unreal --output-on-failure   # ue.bridge-conformance, ue.automation (SKIP until built)
"build/bin/TITAN Studio.app/Contents/MacOS/TITAN Studio" --engine unreal
build/bin/titan-ue-bench --socket <socket> --project /tmp/bench --stress 400 --out renderer-baseline.json
```

`ue.bridge-conformance` launches `UnrealEditor TitanHost.uproject -TitanSocket=…`, waits for `TITAN_HOST_READY`,
and runs `titan-protocol-check --viewport`: the same 12 protocol steps as the reference host, plus 5 frame checks
(shared-memory channel, real rendered content, camera change, edit visible in the frame). Frames are saved as PNG
in `build/ue-conformance/check/`.

### Running the built editor (verified 2026-11-09)
- **Metal Toolchain.** Xcode 26 installs the Metal shader compiler separately. Without it, UnrealEditor quits at
  startup: "cannot execute tool 'metal' due to missing Metal Toolchain". Install it once with
  `xcodebuild -downloadComponent metalToolchain` (about 705 MB), then check it with `xcrun --find metal`.
- **ShaderCompileWorker.** `scripts/ue_build.sh build` builds `TitanHostEditor` and `ShaderCompileWorker` in one UBT
  run. Without the worker the editor quits: "Couldn't launch …/ShaderCompileWorker".
- **First launch.** The editor compiles every shader into the Zen cache, which took 477 s on the 8 GB M3. Later
  launches are ready in about 12 s.
- **Syncing plugin sources.** `scripts/sync_ue_plugins.sh` copies by content and gives changed files the current time,
  so the next `ue_build.sh build` recompiles them. Never use `rsync -a`: it keeps old timestamps, and UBT then
  keeps a stale bridge.
- **Real integration tests:** `ctest --test-dir build -L unreal --output-on-failure`.
- **Studio against Unreal:** `"build/bin/TITAN Studio.app/Contents/MacOS/TITAN Studio" --engine unreal`. Add
  `--smoke-test <dir> --screenshot-dir <dir> --report <file>` for the automated session.
