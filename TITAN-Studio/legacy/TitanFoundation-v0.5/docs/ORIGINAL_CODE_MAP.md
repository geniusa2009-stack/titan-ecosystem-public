# TITAN v0.4 — New original code and boundaries

Every file below was authored as an original implementation in this iteration, drawing only on public architectural patterns and official documentation; no code copied from Unreal, Unity, Bevy, O3DE, Godot, or other engine repositories.

| File | Responsibilities | Limits |
|---|---|---|
| `TitanDataCore/Public/TitanECS.h` | Generational entity IDs, packed sparse component pools, typed queries, deferred commands | Not multi-thread safe; type signatures for lookup are process/toolchain-local, not serialized |
| `TitanDataCore/Private/TitanECS.cpp` | Create / destroy lifecycle and cleanup | Does not represent UE Actors |
| `TitanDataCore/Public/TitanFixedClock.h` | Fixed simulation timestep and bounded catch-up | Floating point results across CPU/compilers are not promised bit-exact |
| `TitanDataCore/Public/TitanVisibility.h` + `Private/TitanVisibility.cpp` | CPU candidate selection, LOD decision and grouping | Bounding spheres only; no GPU occlusion, GPU barriers, Nanite-style virtual geometry, or shader drawing |
| `TitanFoundationRuntime/*/TitanSimulationComponent.*` | UE4.27 Blueprint bridge for logical transform/velocity | No automatic UE actor synchronization or instancing |
| `tests/test_titan_data.cpp` | Generations, compaction, queries, deferred changes, clock, view culling, stress | Unit tests, not GPU or Unreal integration tests |
| `benchmarks/bench_data.cpp` | Local 100k entity CPU update and visibility test | Not a head-to-head benchmark with other game engines |

**Carried over from v0.3:** TitanWorldScheduler, TitanFrameGraph, TitanTaskGraph, TitanSpatialPlanner, TitanLevelStreamingBridge and supporting modules. Those were previously created for TITAN; their code is preserved here.

**Licensing boundary:** This package is independently authored source only. The external `AlexMercer-MA/UnrealEngine-4.27` source contains Epic-owned code subject to Epic's license; a fork does not itself confer unlimited relicensing rights. For commercial standalone redistribution, assess actual written agreement and third-party dependencies. Keep all author and license notices for any third-party code separately incorporated later.
