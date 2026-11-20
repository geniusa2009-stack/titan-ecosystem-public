# Performance Report: TITAN 0.6.0-alpha

All numbers were measured on: Apple M3, 8 GB, macOS 26.5.1, RelWithDebInfo build, 2026-11-05 (0.6) and 2026-11-06 (0.6.1). Each is a
single run unless stated. There are no Unreal measurements yet: the editor is not built (see RENDERER_BASELINE.md). `tools/ue-bench` is ready for that.

## Build
| Measure | Value |
|---|---|
| Clean configure (CMake + Ninja) | 3 s |
| Clean build, everything incl. Studio, `-j4` | 21 s, 0 warnings |

## Engine host (from `titan-protocol-check` report and smoke report)
| Measure | Value | Notes |
|---|---|---|
| `project.create` (write manifest + default scene, open) | 0.82 ms | client-observed round trip |
| `project.open` (engine-side) | 0.37–0.58 ms | 10–29 objects |
| `project.save` (engine-side, atomic write + fsync) | 0.68–0.71 ms | |
| `object.update` + `scene.changed` event round trip | 0.036 ms | blocking client, no GUI |
| Host max RSS at `--version` | 1.5 MB | `/usr/bin/time -l` |

## TITAN Studio viewport (smoke run, 10–29 objects, 2880×1800 Retina window)
| Measure | Value |
|---|---|
| RHI backend | Metal |
| Frame interval, live rendering | 16.6–17.0 ms (≈59–60 fps, **vsync-limited**) |
| GPU time per frame (QRhi timestamps) | 1.2–2.5 ms |
| Render-thread CPU per frame (command recording) | 0.17–0.18 ms |
| Draw calls | 12 (base scene) / 30 (with 18 generated objects) |

## Studio ↔ engine round trip
| Measure | Value | Notes |
|---|---|---|
| Engine launch → protocol negotiated | 0.9–3.6 s | dominated by process launch / first-run macOS checks; varies run to run |
| Idle-UI request round trip (status bar) | 0.47–1.2 ms | observed in screenshots |
| Average round trip during the smoke test (0.6) | 12.9–15.7 ms | replies were read on the GUI thread, which was paced by the render thread's vsync |
| Average round trip during the smoke test (0.6.1, I/O thread) | **0.22–0.30 ms** | same test, 3 runs, 2026-11-06; `avgRoundTripMs` in `smoke-report.json` |
| …GUI delivery (reply handled on the UI thread) | 0.25–0.36 ms | `avgGuiDeliveryMs` |
| Round trip *while rendering continuously* (60 sequential `engine.status`) | 0.023–0.027 ms avg, ≤0.114 ms max | `liveRenderRoundTrip*`, 3 runs |
| …GUI delivery while rendering continuously | 0.03–0.17 ms avg, ≤8.2 ms max | the worst case is one 120 Hz frame |

Fix (0.6.1): `EngineConnection` owns the socket on a dedicated thread. It reads, frames and parses there, and
timestamps the arrival there. High-rate `viewport.frame` events go straight to the frame source without
touching the GUI thread. In this session the viewport ran at 113–121 fps (8.3 ms intervals) on the same Mac. The
display is evidently running at 120 Hz now, where it was 60 Hz in the 0.6 runs, so frame intervals are not
comparable across the two reports.

No claim is made about large-scene performance: the scenes measured have ≤ 30 objects.
