# Renderer Baseline

## 1. Unreal renderer in the supplied source (static audit, V; nothing built or measured)

The supplied tree is UE **5.8.4**, so the renderer is UE5's, not UE 4.27's. Present in
`Engine/Source/Runtime`:

| Module | Files (.cpp) | Notes |
|---|---|---|
| `Renderer` | 380 | Includes `Private/Nanite`, `Lumen`, `VirtualShadowMaps`, `MegaLights`, `Substrate`, `HairStrands`, `RayTracing` |
| `RenderCore` | 84 | Render Dependency Graph (`RenderGraph*.h`), shader compilation infrastructure |
| `RHI`, `RHICore` | — | Backend abstraction |
| `Apple/MetalRHI` | 52 | macOS/iOS backend relevant to the M3 target |

Consequence for planning: the directive's "future targets" (virtualized geometry, improved GI, advanced
shadows) **already exist upstream** in 5.8 as Nanite, Lumen and Virtual Shadow Maps. TITAN renderer
work should start by **measuring those on the target hardware** rather than re-implementing them. Section 7's
protocol applies: baseline → hypothesis → minimal patch → equal-scene comparison → reversible patch.

**Baseline build status: NOT BUILT.** Blockers (V):
- `Setup.sh` has not been run. It downloads several GB of third-party binaries and content from Epic's CDN
  (owner approval needed for network and disk use).
- An UnrealEditor build on 8 GB unified memory is expected to take hours with heavy memory pressure
  (UBT caps parallel actions by memory). Recommended: build on CI or a remote machine with ≥32 GB.
- So no UE frame times, GPU timings or memory figures exist yet. None are reported.

## 2. TITAN Studio viewport renderer (measured, V)

Original QRhi renderer (`studio/src/ViewportItem.cpp`, `studio/shaders/mesh.*`):
- Backend chosen by Qt Quick: **Metal** on this Mac (reported by `QRhi::backendName()`).
- 4× MSAA render target, depth test. Opaque pass, then a grid pass (2 line meshes), ghost pass
  (alpha-blended, back-face culled, no depth write), and selection-box overlay (no depth test).
- One 256-byte std140 UBO slot per draw (dynamic offsets). Lighting is one directional light from the
  scene's first light plus hemispheric ambient and distance fog, computed in linear space with sRGB output.
- No shadows, no PBR materials, no frustum culling, no instancing (see KNOWN_LIMITATIONS.md).

Measured numbers are in PERFORMANCE_REPORT.md.
