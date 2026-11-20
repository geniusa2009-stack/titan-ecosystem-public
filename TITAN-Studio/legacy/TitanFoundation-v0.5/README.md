# TITAN Engine Architecture v0.4 — Independently Authored Code

This additive UE4.27.2 plugin package extends TITAN v0.3 without copying implementations from third-party engine repositories. It does not bundle Epic's source code. The plugin targets the user's existing UE4.27.2 checkout as an optional installation; its source is independently testable with CMake and C++14.

## New native implementation

- `TitanDataCore / TitanECS`: generational entity handles, sparse/packed component storage, safe swap-remove, typed component queries, deferred structural mutations. Single-threaded world; do **not** access concurrently.
- `TitanFixedClock`: bounded fixed-step simulation with interpolation fraction.
- `TitanVisibility`: CPU bounding-sphere frustum culling, screen-space 3-tier LOD selection, deterministic draw candidate batching. This **does not perform GPU rendering** or replace Nanite.
- `UTitanSimulationComponent`: optional UE4 Blueprint-accessible logical entity simulator. Its components are *not* UE actors nor automatically rendered objects.
- All prior v0.3 TITAN systems and UE4 level-streaming integration are preserved.

## Build portable core / validate

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
./build/titan_ecs_benchmark
python3 -m unittest discover -s tests -p 'test_install_*.py' -v
```

## Install additive plugin into your source UE4.27.2 checkout

```bash
python3 scripts/install_into_ue427.py --engine /path/to/UE4.27   # dry-run
python3 scripts/install_into_ue427.py --engine /path/to/UE4.27 --apply
```

The installer **refuses to overwrite an existing plugin**. If upgrading from TITAN v0.3, back up or move your old TITAN plugin directory out of the engine tree after reviewing local changes, then install the new one. It does not modify Epic files.

Detailed comparison, implementation provenance, integration steps and verified test results are in `docs/`. UE4 plugin compilation and hardware rendering are **not** verified in the current build environment. See the license and provenance notes before any public/commercial distribution.


## v0.5 addendum
See README_AR_V05.md and docs/V05_SOURCE_ANALYSIS_AR.md for the original Streaming Telemetry system, offline API contract verification and limitations.
