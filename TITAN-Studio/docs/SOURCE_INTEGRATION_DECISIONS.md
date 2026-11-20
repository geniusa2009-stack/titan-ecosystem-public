# Source Integration Decisions & Source Vault Workflow

## Decisions taken in 0.6

**D-1: Engine base = Unreal Engine 5.8.4. DECIDED by the owner on 2026-11-05** ("Use UE 5.8.4").
The TITAN engine fork is based on the supplied UE 5.8.4 source, not 4.27. Consequences: the directive's
references to "UE 4.27" now read as "UE 5.8.4"; the TITAN v0.5 plugin's 4.27-era API calls are verified
present in 5.8.4 (`ue.source-contract`); the renderer work starts from UE5's Nanite, Lumen and VSM.
Working copy: `engine/UE-5.8.4` (APFS clone of the vault tree), git branch `upstream/ue-5.8.4-baseline`
(unmodified) → `titan/engine` (TITAN patches). Original analysis:
The directive specifies a UE 4.27 fork, but the supplied archive is UE 5.8.4 (verified). UE 4.27 also does
not build with the installed Xcode 26 / Apple Silicon toolchain without substantial patching. TITAN 0.6
therefore puts the editor/engine contract behind a versioned, engine-agnostic protocol. The Unreal-side
bridge is written against the source that was actually supplied (5.8.4). The owner confirmed 5.8.4.

**D-2: TITAN v0.5 is kept, not rewritten.** Its portable core builds and passes 4/4 tests unchanged, and
its UE API calls exist in 5.8.4. It is built and tested in CTest as-is. It is not yet wired into the 0.6
runtime: there is no consumer for streaming or ECS in the vertical slice, and wiring it in without one
would be speculative.

**D-3: Process boundary between editor and engine.** Qt 6 and Unreal have incompatible application loops,
allocators and ABI expectations, and Unreal cannot yet be built here. TITAN Studio talks to an engine
*process* over local IPC. This gives crash isolation (verified: Studio detects host exit and offers restart),
no ABI coupling, and the same contract for the reference host and the Unreal bridge.

**D-4: Reference engine host.** To get a genuine end-to-end slice before UE builds,
`titan-engine-host` (original C++) owns the canonical scene, undo history and persistence, and loads
extensions. It is TITAN's own runtime, not a mock: the integration and UI tests drive it as a real process.
It is not Unreal and does not use the Unreal renderer.

**D-5: Viewport rendering in Studio via QRhi (Metal on macOS).** (For the Unreal backend this is superseded by D-10.) This is the measured fallback from the
directive's Section 4 option list ("image/frame transport fallback" / "alternative proven native
integration"). Embedding an Unreal viewport needs a running UE build. Planned path: the UE editor renders
offscreen, and frames are shared via IOSurface (macOS) / shared handles, synchronized over the same protocol.
See EDITOR_INTEGRATION.md.

**D-6: Qt module selection for licence compatibility.** Qt Quick 3D is not used (GPL-3.0/commercial).

**D-7: Renderer strategy = Advanced Unreal Renderer Fork. DECIDED by the owner on 2026-11-06.** TITAN enhances
UE 5.8.4's renderer rather than replacing it. Nanite, Lumen, Virtual Shadow Maps, RDG, GPU Scene and
streaming are benchmarked first on the target hardware (`tools/ue-bench`). A TITAN renderer change needs a
measured reason and an equal-scene before/after comparison, and must be a reversible patch on `titan/engine`.

**D-8: Engine fork at an ASCII-only path.** The fork moved from `محرك/engine/UE-5.8.4` to
`~/TitanEngine/UE-5.8.4` (same APFS volume, instant `mv`, no copy). The old path is a symlink. Unreal's
toolchain (UBT, shader compiler workers, ISPC, response files) is not verified with non-ASCII paths, and a
multi-hour build should not depend on it.

**D-9: First Unreal build is project-scoped and without debug info.** `TitanHostEditor` with
`DisableEnginePluginsByDefault` and `-NoDebugInfo` fits the measured disk budget (35 GB headroom) on this Mac.
A full `UnrealEditor` build with all default plugins and full debug info is for a larger build machine.

**D-10: Unreal viewport frames via scene capture + shared memory (implemented in bridge v2, not yet run).**
macOS has no `-RenderOffscreen` for Unreal 5.8.4 (verified: only Linux/Windows/Null implement it). The bridge
renders TITAN Studio's camera with a transient `ASceneCapture2D` (`SCS_FinalColorLDR`, persistent view state for
TSR/Lumen/VSM history) into a render target. It reads the target back with `FRHIGPUTextureReadback`, without
stalling the game thread, into a 3-slot seqlock ring in POSIX shared memory (`TitanFrameChannel.h`). It
announces each frame on the protocol socket. Studio uploads the frame as a texture and draws its overlays on
top. Zero-copy IOSurface sharing is the planned optimisation once copies are measured.

## Source Vault workflow (for each future partner archive)

1. Copy to `vault/originals/`, record name, size and SHA-256 in `SOURCE_INVENTORY.md`.
2. Check entries (absolute paths, `..`, symlinks, compression ratio), then extract read-only to `vault/extracted/<id>/`.
3. Read the licence and agreement documents and add a column to `LICENSE_AND_RIGHTS_MATRIX.md`.
4. Scan for secrets (record file names only) and inventory `.tps`/LICENSE/NOTICE files.
5. Map the architecture and compare with existing TITAN subsystems. Record overlaps as a new decision `D-n`.
6. Choose an approach: adapter extension (preferred), isolated module, fork patch, or original implementation.
7. Implement on a `feature/<partner>-<component>` branch with tests and benchmarks.
8. Present the diff and test results, and merge only on explicit approval.

Never replace a working subsystem only because a partner supplied a similar one; require measured benefit.
