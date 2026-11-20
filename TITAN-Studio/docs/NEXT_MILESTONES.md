# Next Milestones (ordered by dependency)

1. ~~Owner decision D-1~~ **Done:** UE 5.8.4 is the fork base (2026-11-05). Still wanted: agreement documents
   for the rights matrix.
2. **UE baseline build** on CI or a remote ≥32 GB Mac: run Setup.sh, build UnrealEditor, record build
   time, errors and memory. Commit an `upstream/ue-5.8.4-baseline` reference (private).
3. **Compile and conformance-test TitanStudioBridge** with `titan-protocol-check`. Add events, create,
   delete and pick.
4. **Viewport frame sharing** UE → Studio via IOSurface → QRhi native texture import. Measure latency
   against the current in-Studio renderer.
5. **Studio I/O thread** for EngineClient (removes the 12–16 ms RTT coupling to vsync). Target is under
   2 ms during live rendering.
6. **Engineering Agent foundation:** `git worktree` manager, plan/diff/test/approve pipeline with
   read-only default and audit log, and an AI Lab workspace UI.
7. **World Builder v2:** terrain heightfield operator, road graph, per-region regeneration, recipe history
   UI.
8. **Out-of-process extension host** for untrusted or partner extensions, with capability enforcement at
   the protocol boundary.
9. **Windows/Linux:** named-pipe transport, `LoadLibrary`, CI matrix, and only then mark platforms
   supported.
10. **Renderer track on UE:** baseline Nanite, Lumen and VSM timings on M3, then one measured, reversible
   optimization.
11. SBOM generation from UE `.tps` files; packaging with LGPL notices for Qt.
