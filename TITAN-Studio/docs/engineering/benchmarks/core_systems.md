# Core systems — benchmark specifications

Specifications only. **No results have been measured**; nothing here claims a frame time or throughput.
Run on a Release build, pinned to one core, median of 5 runs after 1 warm-up run. Record the CPU and compiler.

## Physics reference simulation (`titan/physics.h`)

| ID | Scenario | Measure | Budget to check against |
|----|----------|---------|-------------------------|
| PHY-1 | 1,000 dynamic spheres (r 0.5) falling onto one static ground box, 600 steps of 1/60 s | mean and p99 `step()` time; peak `contacts().size()` | p99 < 2 ms |
| PHY-2 | Same as PHY-1 after all bodies sleep | mean `step()` time | at most 10% of PHY-1 mean |
| PHY-3 | 4,096 static boxes on a grid, 10,000 random rays (length 100) | mean `raycast()` time | linear scan; becomes the baseline for the spatial index |
| PHY-4 | 1,000 bodies stacked in a 10x10x10 column (worst-case overlap) | `candidatePairs`, `skippedPairs`, step time | no unbounded growth; `skippedPairs` reported, not hidden |

Repeatability check (all scenarios): two runs with identical input must produce identical `toJson()` dumps.

## Audio voice management (`titan/audio.h`)

| ID | Scenario | Measure | Budget to check against |
|----|----------|---------|-------------------------|
| AUD-1 | 4,096 active 3D voices, 64 real, NullAudioBackend, listener moving every update | mean and p99 `update(1/60)` time | p99 < 0.5 ms |
| AUD-2 | 1,000 `play()` calls per frame into full concurrency groups (StopLowestPriority) | mean `play()` time | linear in voices; note the group scan cost |
| AUD-3 | Same as AUD-1 with voices oscillating across the real/virtual boundary | backend start/stop calls per update | equals the ranking churn; no duplicate starts |

## Spatial index (`titan/spatial.h`)

| ID | Scenario | Measure | Budget to check against |
|----|----------|---------|-------------------------|
| SPA-1 | Insert 100,000 boxes (mixed sizes, coordinates near 1e9) | total insert time; `height()`; `nodeCapacity()` | height <= 2.5 log2(n) |
| SPA-2 | 10,000 objects, 60% moving by < margin per frame, 1 frame | `update()` time; fraction returning true | most moves in place |
| SPA-3 | 10,000 objects, 10,000 random box / sphere / ray / frustum queries | mean query time vs brute force (PHY-3 baseline) | >= 10x faster than brute force at 10,000 objects |
| SPA-4 | Unload one streaming group of 1,000 of 100,000 objects | `removeGroup()` time | O(n) scan documented; revisit if > 1 ms |
