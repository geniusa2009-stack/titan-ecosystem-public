# Known Limitations (0.6.0-alpha)

**Engine**
- The engine process is TITAN's reference host, **not** the Unreal Engine. The UE 5.8.4 baseline is
  unbuilt, and the Unreal bridge is written but not compiled or tested.
- Scene content is limited to primitives (cube, plane, sphere, cylinder), groups, one directional light
  proxy and camera proxies. There is no asset import, materials, textures, physics or animation.
- Native extensions run in-process with full privileges. The manifest is a compatibility check, not a
  sandbox. There is no out-of-process extension host yet, and no extension timeouts.
- Windows transport (named pipes) and extension loading are not implemented.

**Studio**
- Viewport: no shadows, PBR, frustum culling, instancing or transparency sorting. There are no transform
  gizmos (move with ⌥-drag on the ground plane, or use Inspector fields and label scrubbing).
- Panels resize through SplitView but cannot be freely docked or undocked.
- Arabic: language toggle, RTL mirroring and about 60 translated chrome strings. Most body text is
  English-only, and RTL layouts have not been fully reviewed.
- Code, Materials, Cinematic, AI Lab and Source Engineering workspaces are explicit "Planned" shells.
- The TITAN Engineering Agent (worktree isolation, diff/approval UI) is **not implemented** in 0.6.
  Only its architecture is specified.
- While live rendering, IPC replies are delayed by the GUI thread's vsync-paced sync (12–16 ms average).
  This is fixable by moving I/O to a worker thread.
- Multi-selection edits apply to the first selected object only.
- Unsaved changes are lost if the engine process crashes. Studio reports this and can restart and reopen
  the last saved state, but there is no autosave or journal yet.
- QRhi is semi-public Qt API (`GuiPrivate`) and may need adjustments on Qt minor upgrades.
