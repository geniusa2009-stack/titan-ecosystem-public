# TITAN company-grade architecture roadmap

## Current actual modules (v0.2)

```text
UE4.27 project (if licensed)         Portable C++14 core, no Unreal headers
├── TitanFoundationRuntime           ├── TitanWorldScheduler  (I/O request plan + ack)
│   ├── TileStreaming (v0.1)          ├── TitanFrameGraph       (hazards, DAG, lifetimes)
│   ├── AdaptiveQuality (v0.1)        └── TitanTaskGraph        (resource conflicts, waves)
│   └── WorldOrchestrator (v0.2)                  ↑
└── TitanOrchestrationCore  ---------------------┘
```

### State machine: streaming

`Missing → Loading (request sent) → Resident (load acknowledged) → Unloading (request sent) → Missing (unload acknowledged)`

Failures: `Loading → Missing`, `Unloading → Resident` on explicit failure. Reserved memory is held for the whole loading/resident/unloading lifetime. Predictive prioritization affects selection but doesn't permit tiles outside the present load radius; hysteresis avoids thrash. Pending I/O is never secretly treated as completed. When a scene is deleted/cancelled externally, coordinate cancellation and `Reset()` appropriately.

### Frame graph semantics

Passes are declared in semantic order, with per-resource read/write intents and optional explicit `after` edges. The compiler builds hazard edges (RAW, WAR, WAW), validates that transients are written before reads, detects cycles, computes first/last uses, then greedily reuses compatible non-overlapping allocations. It does **not** synchronize GPU queues, construct textures, or submit shader work. Those belong in a platform RHI executor with actual resource fences and transitions.

### Task graph semantics

A read/write resource-set conflict makes a deterministic edge in insertion order. Independent ready tasks form waves, optionally dispatched to native background threads with bounded concurrency. Declare true access sets. Unreal UObjects and rendering APIs generally must remain on their owning threads; call the portable graph with native thread-safe work only.

## Next milestones and acceptance criteria

1. **UE4.27 integration**: compile plugin in authorized full source build; run AutomationSpec test; hook game-controlled streaming requests to `ULevelStreamingDynamic` or content manager callbacks; ensure ack even on failure.
2. **Telemetry**: load request, IO-start, IO-end, GPU-upload, visible-ready timestamps, `P50/P95/P99`; memory pressure and frame spikes.
3. **Asset system**: async state machine + cancellation + ref-counted handles, source asset hash, content-addressed cache, hot reload, dependency graph.
4. **Rendering backend**: `RenderDevice` interface with Metal first on M3, Vulkan/D3D12 adapters later; RDG executor, CPU/GPU hazard barriers, shader reflection, descriptors.
5. **Scene/ECS integration**: handles with generations, sparse-set archetype experiments, transform changes tracked, workloads independently benchmarked vs existing UObject/component system.
6. **Company-grade CI/CD**: Linux/macOS/Windows build matrix, sanitizers, fuzzing, golden scenes, binary compatibility tests, CLA & NOTICE tracking, reproducible release builds.
7. **Editor**: native asset browser, scene inspector, materials/graph editor, plugin SDK and scripting API, documentation generator.
8. **Performance**: define concrete target hardware and scenes; compare quality and latency under equivalent visual features, not across different demo levels.

## macOS M3 / 8 GB constraints

Use portable unit tests and source indexing locally. UE4 full native compilation may require substantial disk and memory; a supported Mac build host with more RAM or hosted CI is strongly recommended. Avoid assuming Nanite/Lumen-like fidelity from just having a graph compiler.
