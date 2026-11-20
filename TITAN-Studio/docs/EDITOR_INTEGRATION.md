# Editor ↔ Engine Integration (titan.editor protocol v1)

## Transport
- Unix domain socket created by the server with mode 0600. Peers running as a different UID are rejected
  (`getpeereid`). Windows named pipes are **not implemented**.
- Frame: 4-byte big-endian payload length followed by a UTF-8 JSON object. Frames over 16 MiB close the
  connection.
- Studio launches the engine and waits for the stdout line `TITAN_HOST_READY <path>`, then connects.
  The engine exits when Studio exits. Two engines speak the same protocol:
  - reference host: `titan-engine-host --socket <tmp> --parent-pid <pid> --extensions <dir>`
  - Unreal: `UnrealEditor TitanHost.uproject -TitanSocket=<tmp> -TitanParentPid=<pid> -TitanMinimizeEditor
    -nosplash -unattended -NoSound` (Studio `--engine unreal`, Launcher "Engine" switch, or the command palette).
- Studio reads the socket on a dedicated I/O thread (`EngineConnection`). Requests are queued to it.
  Replies and events are parsed there and delivered to the GUI thread. `viewport.frame` events go straight
  to the frame source.

## Messages
`{"id":n,"method":"m","params":{}}` → `{"id":n,"result":{}}` | `{"id":n,"error":{"code","message"}}`;
events: `{"event":"name","data":{}}`, sent only to negotiated clients.

Error codes: -32600 invalid request, -32601 unknown method, -32602 invalid params, -32603 internal,
1000 not negotiated, 1001 no project, 1002 not found, 1003 I/O, 1004 version mismatch,
1005 extension error, 1006 conflict (unsaved changes, stale proposal).

| Method | Params | Result |
|---|---|---|
| `session.hello` | `client`, `protocol:{min,max}` | `protocol`, `engine{product,version,backend,platform}`, `capabilities`, `state` |
| `engine.status` | — | state + `uptimeMs`, `objectCount`, `rssBytes`, `metrics{requests,errors,avgRequestUs,maxRequestUs,lastOpenMs,lastSaveMs}` |
| `engine.shutdown` | — | `{}` then the host exits |
| `project.create` / `project.open` | `path` (+`name`), `force?` | `project`, `scene`, `editor{camera}`, `state`, `openMs` |
| `project.save` | — | `path`, `saveMs`, `state` |
| `project.close` | `force?` | state (1006 if dirty and not forced) |
| `scene.query` | — | `name`, `revision`, `environment`, `objects[]`, `recipes[]` |
| `object.get` | `id` | object + `worldMatrix[16]` |
| `object.update` | `id`, `set{name,transform{position,rotation,scale},color,visible,intensity}`, `coalesce?` | `object`, `revision` |
| `object.create` | `name?,type,shape,parent?,transform?,color?` | `id`, `object` |
| `object.delete` | `ids[]` (descendants included) | `deleted[]` |
| `selection.set` | `ids[]` | `ids` |
| `scene.pick` | `origin[3]`, `direction[3]` (world) | `id` or null, `distance` |
| `edit.undo` / `edit.redo` | — | `applied`, `label`, `state` |
| `editor.setState` | `camera{yaw,pitch,distance,target}` | `{}` (written to `Saved/editor.json`) |
| `extensions.list` | — | `extensions[]` (manifest + `status` + `problems`), `platform` |
| `assets.list` | — | `assets[]` (`id`, `path`, `importer`, `status`, `error?`, `sourceSize`, `metadata`, `dependencies[]`); needs capability `assets` |
| `assets.refresh` | — | `stats` (`scanned`, `hashed`, `imported`, `reused`, `failed`, `missing`, `renamed`) + `assets[]`; emits event `assets.changed` (stats) |
| `prefab.create` | `id`, `name?`, `overwrite?` | `prefabId`, `path` (Assets-relative), `objects`; saves `Assets/Prefabs/<name>.tprefab`, links the source objects (one undo step); capability `prefabs` |
| `prefab.instantiate` | `path`, `parent?`, `position?` | `root`, `ids[]`, `prefabId`; fresh ids, hierarchy kept, objects linked via `prefab {id, local}`; one undo step |
| `extensions.invoke` | `extension`, `command:"describe"` | extension schema (read-only commands only) |
| `worldbuilder.preview` | `extension`, `recipe{}`, `recipeId?` | `proposalId`, `objects[]`, `produced`, `replaces[]`, `warning?` |
| `worldbuilder.approve` / `reject` | `proposalId` | `recipeId`, `groupId`, `created[]` / `rejected` |
| `viewport.configure` *(Unreal)* | `width`, `height` (engine pixels) | `shm`, `width`, `height`, `slots`, `version`, `format:"bgra8-srgb"` |
| `viewport.camera` *(Unreal)* | `eye[3]`, `forward[3]`, `fovY` (vertical, degrees), `view[16]?`, `revision` | `revision` |
| `viewport.render` *(Unreal)* | `continuous` | `continuous` (off: frames after each change until temporal effects settle) |
| `unreal.setCVar` *(Unreal)* | `name` (`r.*` only), `value` | `name`, `value` (renderer A/B for benchmarks) |
| `unreal.window` *(Unreal)* | `visible` | minimises/restores Unreal's own windows |

Events: `scene.changed {revision, reason, upserted[], removed[], recipes[], state}`,
`selection.changed {ids}`, `project.opened`, `project.state`, `project.closed`,
`viewport.frame {frame, slot, width, height, cameraRevision, captureToReadyMs}` (Unreal).

Capability contract (`core/include/titan/capabilities.h`): every capability has a version and the methods it covers. `session.hello` returns `capabilities` and `capabilityVersions` ({name: version}); a backend that only lists names counts as version 1. A request for a method whose capability the backend does not advertise is answered with error **1007 `kUnsupported`** (unknown methods stay -32601). Studio refuses such calls itself, without sending them, and `EngineClient::supports(name, minVersion)` gates features.

Capabilities (`session.hello`): reference host `project, scene, selection, undo, pick, extensions,
worldbuilder.preview`, `assets`, `prefabs`; Unreal bridge `project, scene, selection, undo, pick, viewport.frames, unreal.levels`.

## Conformance
`titan-protocol-check --socket <path> --workdir <dir> [--shutdown] [--report file] [--viewport]` exercises
hello → create → select → update+event → get → undo/redo → save → close → reopen → status (12 steps). It
runs against the reference host in CTest (`host.protocol-conformance`, passing). With `--viewport` it adds 5
frame checks for engines that render: the shared-memory channel maps, shader compilation finishes, a frame
with real content arrives (luma std-dev > 3), a camera change re-renders the image (mean diff > 2), and an
object edit shows up in the rendered frame. `ue.bridge-conformance` runs all 17 steps against the real
Unreal editor. **It has not run yet: the editor is not built.**

## Unreal bridge v2 (`unreal/Plugins/TitanStudioBridge`): written, NOT COMPILED YET
Game-thread only (`FTSTicker`); the render thread is touched only through `ENQUEUE_RENDER_COMMAND` in
`TitanBridgeFrames`. Activation needs `-TitanSocket` (otherwise dormant). It covers:

| Area | Implementation |
|---|---|
| Projects | TITAN project dir with `project.titan` (`backend:"unreal"`, `level`) ↔ Unreal level `/Game/TitanProjects/<Name>_<crc>/<Name>`. `project.create` loads Unreal's `Template_Default` (floor, sun, sky, fog) as a template, adds the TITAN starter primitives (`/Engine/BasicShapes`) and saves with `UEditorLoadingAndSavingUtils::SaveMap`. `project.open` also accepts `level` to open any existing Unreal level. |
| Objects | every outliner-listed, editable actor; ids `CityHash64(ActorGuid) & (2^53-1)` (persisted with the level); type light/camera/mesh/group, shape from the static mesh, `unrealClass`; local transforms via the root component. |
| Coordinates | `TitanCoordinates.h`: positions `titan = (Y, Z, -X)/100`; rotations converted exactly as `R_t = M·R_ue·M⁻¹` (matrix-based, any rotation), checked in `ue.coordinates` and against `FRotationMatrix` in the engine automation test. |
| Edits / undo | `FScopedTransaction` per edit; a `coalesce` gesture keeps one `GEditor->BeginTransaction` open until the key changes, another request arrives, or 0.5 s idle → one Unreal undo step. `edit.undo/redo` = `UndoTransaction/RedoTransaction`; labels from the transaction buffer. |
| Echo of Unreal-side edits | `OnActorMoved`, `OnLevelActorAdded/Deleted`, `OnObjectPropertyChanged`, `PostUndoRedo`, `USelection::SelectionChangedEvent` → `scene.changed` / `selection.changed` once per tick. |
| Picking | `LineTraceSingleByChannel` (complex collision) in the editor world. |
| Frames | see decision D-10: transient `ASceneCapture2D` → `UTextureRenderTarget2D` (BGRA8 sRGB) → `FRHIGPUTextureReadback` (≤ 2 in flight) → 3-slot seqlock ring in POSIX shm (`TitanFrameChannel.h`) → `viewport.frame`. |
| Status | Unreal frame / game / render / RHI / GPU ms, shaders remaining, renderer CVars, RHI texture memory, frame stats. |
| Not supported yet | colour edits (Unreal materials), World Builder and extensions, hierarchy re-parenting, World Partition levels, multi-client. |

### Studio side
`RemoteFrameSource` maps the channel read-only. The Qt Quick render thread copies the newest complete frame
under the seqlock and uploads it as a BGRA8 `QRhiTexture`. It draws it with a full-screen triangle
(`frame.vert/frag`), then draws selection outlines on top. Until the first Unreal frame arrives, Studio
draws its own proxy view of the same scene. The engine renders at `engineRenderScale` × physical pixels
(default 0.5 = one engine pixel per point on Retina). Covered by `studio.frame-channel`: reader against a
protocol-faithful writer, 6 cases including torn frames.
