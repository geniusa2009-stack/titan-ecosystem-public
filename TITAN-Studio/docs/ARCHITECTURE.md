# TITAN 0.6.1 Architecture (as implemented)

```
┌──────────────────────── TITAN Studio (Qt 6 / QML, C++) ────────────────────────┐
│ Launcher · Workspaces (World ✔, Performance ✔, Extensions ✔, 5 shells)         │
│ Hierarchy ─┐   Inspector ─┐   World Builder panel ─┐   Command Palette          │
│            ▼              ▼                        ▼                            │
│      SceneModel (read-only mirror)  ◄── events ──  EngineClient ─ I/O thread    │
│      ViewportItem (QQuickRhiItem → QRhi → Metal/D3D/Vulkan)   │ supervises      │
└───────────────────────────────────────────────────────────────┼─────────────────┘
                     titan.editor v1: 4-byte BE length + JSON    │ Unix domain socket (0600, same-uid)
┌──────────────────────── titan-engine-host (C++20) ─────────────┼─────────────────┐
│ poll() loop · per-connection negotiation · event broadcast     ▼                 │
│ EngineSession ── authoritative ProjectInfo + Scene + UndoStack + selection       │
│      │                 │                     │                                    │
│ titan_core: Json · Scene/ObjectId · SceneEdit/UndoStack · project I/O (atomic)    │
│             picking · meshes · protocol framing · POSIX IPC                       │
│ ExtensionHost: manifest validation → dlopen → C ABI (titan/extension_api.h)       │
│      └── titan.procedural.scatter (World Builder operator)                        │
└──────────────────────────────────────────────────────────────────────────────────┘
          same protocol ▼ (written; awaiting the UE build)
┌──────────── Unreal Editor 5.8.4 (TitanHost project) + TitanStudioBridge ─────────┐
│ FSession (game thread, FTSTicker) ── levels, actors, FScopedTransaction, undo     │
│ FFrameChannel: ASceneCapture2D → render target → async GPU readback (RT thread)   │
│      └── POSIX shared memory, 3-slot seqlock ring ──► Studio RemoteFrameSource    │
│                                                          → QRhi texture + overlays│
└──────────────────────────────────────────────────────────────────────────────────┘
```

## Boundaries

| Layer | Owns | Must not |
|---|---|---|
| `titan_core` (static lib, no deps) | value types, serialization, transactions, wire framing | depend on Qt or UE |
| Engine host / `EngineSession` | **canonical** project, scene, undo history, selection, proposals | trust client input (all params validated) |
| Extensions | pure functions from recipe → proposal | mutate the scene (host commits after approval) |
| Studio | presentation, input, camera, preview ghosts | mutate scene state locally (every edit is a request) |

## Key contracts

- **Identity:** `ObjectId` = uint64 below 2^53, allocated monotonically and never reused, stable across
  save/reload (verified by tests). Clients never receive pointers.
- **Transactions:** every mutation is a `SceneEdit` (before/after snapshots of the objects and recipes it
  touches). The engine pushes it on the `UndoStack`. Continuous gestures carry a `coalesce` key and merge
  into one undo step. Merging never crosses save, undo or redo boundaries. Dirty state is tracked against
  the save point, including saves that become unreachable after branching.
- **Persistence:** `project.titan` and `Scenes/*.tscene` (JSON, insertion-ordered, versioned
  `formatVersion`). Writes are atomic (temp file → fsync → rename). Paths read from disk cannot escape
  the project. Per-user editor state (camera, selection) is kept separately in `Saved/editor.json`.
- **Coordinates:** right-handed, Y-up, metres; Euler degrees R = Ry·Rx·Rz. `titan_core::Mat4` is used by
  both engine (picking) and Studio (rendering), so they agree exactly. The UE bridge converts from UE's
  Z-up, left-handed, centimetre system.
- **World Builder:** `worldbuilder.preview` → extension `generate` (deterministic PCG32) → proposal held
  by the engine, rendered as ghosts in Studio → `approve` commits one undoable `SceneEdit`. Generated
  objects carry a `generator {extension, recipeId}` tag under a group, and the recipe is stored in the
  scene. Regenerating with a `recipeId` replaces only that recipe's output.

## Threads

- Engine host: single-threaded. All state is touched on one thread, so it needs no locks.
- Studio: the GUI thread owns EngineClient and SceneModel. `EngineConnection` owns the socket on its own
  I/O thread (requests are queued to it, replies and events come back queued). The Qt Quick render thread
  runs `ViewportRenderer`. Data crosses threads only in `synchronize()` (GUI blocked), as lock-free stats
  atomics read after `frameSwapped`, or through `RemoteFrameSource` (a mutex for the mapping, a seqlock
  for the pixels).
- Unreal bridge: UObjects only on the game thread. The render thread owns the readbacks and copies the
  pixels into shared memory. Completed frames return through an SPSC queue. The mapping is released only
  after `FlushRenderingCommands()`.

The protocol method catalogue is in `EDITOR_INTEGRATION.md`.
