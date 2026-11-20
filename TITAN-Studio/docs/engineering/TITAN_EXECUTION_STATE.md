# TITAN execution state

This is the checkpoint for resuming work. Facts only; update it at each milestone. Last update: 2026-11-16 (UTC).

## Where things are
- **TITAN repository:** branch `titan/stage-preflight` (draft PR #2, stacked on PR #1, `ci/github-actions`). Both
  are unmerged; merging needs the owner's approval.
  - PR #3 (`fix/frame-geometry-overflow`, targets PR #2) was reviewed and validated locally (12/12); not merged.
  - PR #4 is a design reference. Issue #5 is the roadmap.
- **Engine fork:** `~/TitanEngine/UE-5.8.4`, branch `titan/engine`, local commits only (latest: bridge sync
  `01afab707`).
- **Built and verified:**
  - `UnrealEditor` and the `TitanHostEditor` target are built (Development, arm64, `-NoDebugInfo`).
  - `ShaderCompileWorker` is built.
  - The Metal Toolchain 17B54 is installed.
  - The shader cache (Zen DDC) is warm, so the editor is ready in about 12 s.
- **Running processes:** none should be running between sessions. Check with
  `pgrep -fl "UnrealEditor|UnrealBuildTool|TITAN Studio"`.

## Milestone status
- **Priority Zero (real Unreal vertical slice): integrated, with one open stability defect.**
  - **Done:**
    - The build links.
    - The bridge loads (`TITAN_HOST_READY`).
    - `ue.bridge-conformance` passes 18/18 and `ue.automation` 2/2.
    - The Studio session passes 29/32, with 3 SKIP for World Builder, which the bridge does not support.
  - **Open:**
    1. 1 of 3 timed Studio sessions failed with Unreal ticking at about 1 Hz under memory pressure (swap about
       5 GB). The cause is unproven.
    2. Viewport performance is 4–9 engine fps with 0.2–1.1 s latency at 802×828.
    3. World Builder is not implemented in the bridge.

## Next executable actions, in order
1. **Stability under memory pressure.** Reproduce the slow-tick failure with per-second sampling: Unreal frame
   counter, App Nap state via `NSProcessInfo` activity, `vm_stat`. Test two candidate fixes separately:
   (a) a bridge-side `beginActivityWithOptions(NSActivityUserInitiated | NSActivityLatencyCritical)` while a
   session is connected; (b) a low-memory render profile, i.e. screen percentage or Lumen/VSM via `unreal.setCVar`.
   Adopt only what the measurements support.
2. **Viewport performance baseline.** Use `tools/ue-bench`, with a fixed camera path and 3 runs. Measure engine fps,
   GPU ms and latency against render scale and the Lumen/VSM settings.
3. **Asset pipeline** (Issue #5, milestone 1). Design it against the existing core contracts, then implement it on a
   feature branch.

## Owner approvals pending
None. Ask before: merging any PR, downloads, system changes, deletions outside build outputs, and installing
large local AI models.

## 2026-11-16 checkpoint
- **Disk blocker:** the internal SSD has 11–12 GB free, below the 40 GB needed to start Unreal. Consumers are user data or apps: `~/Desktop/كل المشاريع` 52 GB, Claude VM bundles 17 GB, Docker 15 GB, swap 6 GB. Every real-Unreal run is blocked until the owner frees space. Issue #8 has the details.
- **Issue #8** (PR #9, branch `perf/ue-stability-8`, draft):
  - The failed run's stalls coincide with UnrealEditor's RSS collapsing to 16–200 MB under 4–5.4 GB of swap. Correlation only; not proven.
  - Added `tools/ue_runtime_monitor.py` (per-second paging counters, safe abort), monitor wiring in conformance, and `tools/run_studio_unreal_sessions.sh` (3-run gate). Not yet run against Unreal.
- **Asset pipeline M1** (PR #10, branch `feature/asset-pipeline-m1`, draft):
  - Core database and importers.
  - Engine-host `assets.*`, Studio Assets workspace.
  - `core.assets` 16/16 (mutation-checked); `ctest -LE unreal` 15/15; benchmark in `docs/ASSET_PIPELINE.md`.
- **Next:**
  1. Once at least 40 GB is free (preferably 60+), run the Issue #8 baseline and experiment matrix with the monitor.
  2. Otherwise continue M2, runtime capability and telemetry contracts (Issue #5).

## 2026-11-16 (later) checkpoint
- **Review fixes:**
  - PR #10: dependency deletion or rename invalidates dependents, and a source that reappears recovers.
  - PR #9: the abort stops the whole process tree.
- **Feature PRs, all drafts stacked in order #10 → #11 → #12 → #13 → #14 → #15, none merged:**
  - prefabs (in #10);
  - scene editing (#11): duplicate, group/ungroup, snapping;
  - autosave and crash recovery (#12);
  - Assets thumbnails and kind filter (#13);
  - scene templates (#14);
  - extension enable/disable and rescan (#15).
- **Unreal is still blocked** by disk space (under 40 GB free). None of these features exist in the Unreal bridge yet; each one is capability-gated.
- **Next:**
  - Unreal-side `assets`, `prefabs` and `edit.*` in the bridge, once disk space allows.
  - Multi-object Inspector editing.
  - Model and audio previews.

## 2026-11-17 checkpoint
- **Review fixes:**
  - PR #12: a failed Recover/Discard keeps the recovery offer.
  - PR #14: the New Project template picker is UI-tested.
  - Both are merged up the stack without force-push.
- **PR #16** (top of the stack): versioned capability contract with error 1007 `kUnsupported`, and Studio refuses unsupported methods locally.
- **UI smoke:** 44 steps cover prefabs, assets, duplicate/group commands, recovery, templates and the contract.
- **Merge check:** PR #9 and PR #3 merge cleanly with the stack.
- **Unreal:** every bridge item is NOT TESTED — DISK.
- **Next:** with disk space, add `capabilityVersions` and 1007 to the Unreal bridge, then implement `assets`, `prefabs` and `edit.*` there. Without it, multi-object Inspector editing.

## 2026-11-17 (later) checkpoint — editor power-up
- **Ownership rule** added to `CLAUDE.md`: the authorized Unreal fork is TITAN's modifiable foundation.
- **Draft PRs**, stacked #16 → #17 → #18 → #19 → #20 → #21 → #22, none merged:
  - #17 multi-object Inspector (`edit.multi`);
  - #18 hierarchy drag-and-drop parenting (`edit.reparent`, `Transform::fromMatrix`);
  - #19 smart outliner (`edit.lock`, type filter, selection actions);
  - #20 camera tools (smooth framing, `camera.bookmarks`);
  - #21 copy/paste (`edit.clipboard`, `titan.clipboard` v1);
  - #22 undo history panel (`edit.history`: `edit.history`, `edit.goto`).
- **Tests:** `ctest -LE unreal` 16/16. The host integration suite has 17 tests; UI smoke has 59 PASS and 1 SKIP. The new guards are mutation-checked.
- **Unreal:** none of these capabilities is in the bridge yet, so each is hidden or refused there. NOT TESTED — DISK.
- **Next:**
  - with disk space, implement these capabilities in the Unreal bridge;
  - otherwise, model and audio previews in Assets.

## 2026-11-17 SAFE PAUSE (owner order)
- **Paused** because free disk is about 4–5 GB. The cause is about 15 GB of macOS swap; TITAN's `build/` is 120 MB.
  - No builds, tests or disk-heavy work until space is recovered; nothing deleted, no cleanup attempted.
  - Unreal stays blocked below the 35 GB floor (40 GB to start the editor).
- **State:** all work is committed and pushed (working tree clean). Top branch `feature/undo-history`.
  - Draft PR stack #16 → #22, none merged.
  - No TITAN or Unreal processes running.
- **Open finding, PR #21** (owner review, not fixed yet):
  - `Clipboard::copy` and `Clipboard::paste` silently keep the old transform when `Transform::fromMatrix` fails (shear), which yields a "successful" copy or paste with the wrong world placement.
  - Fix on `feature/copy-paste`: refuse explicitly with a clear validation error. The engine must map it to `kInvalidParams` and commit nothing.
  - Add core regression tests for a non-decomposable root copy and for paste-into-parent, then merge up to #22.
- **Resume order (lightweight first, once disk is safe):**
  1. PR #21 finding.
  2. Unreal bridge capabilities, only once ≥ 40 GB is free.

## 2026-11-17 GITHUB-ONLY CORE SYSTEMS (disk still critical)
- **Done remotely via the GitHub API.** Nothing was built or run, so every test below is **NOT VERIFIED**; the only check run was `clang++ -fsyntax-only` with the project warnings.
- **PR #21 finding fixed:** the shear refusal plus the cut-confirmation review. Merged up to #22.
- **Draft stack #23 → #32 (core only, no capability advertised, no Studio UI):**
  - #23 components;
  - #24 Play/Edit session;
  - #25 visual scripting;
  - #26 animation timeline (plus the owner's non-finite-time fix, merged up the stack);
  - #27 input mapping;
  - #28 materials;
  - #29 gameplay event bus;
  - #30 physics core (model and reference solver; Chaos stays the Unreal backend);
  - #31 audio core (no playback; backend interface for Unreal Audio);
  - #32 spatial index (dynamic AABB tree).
- **Benchmark specifications:** `docs/engineering/benchmarks/core_systems.md` (specifications, no results).
- **Next, final phase:** build and run all suites, then do the integration review, protocol exposure, the Unreal bridge (once ≥ 40 GB is free) and end-to-end validation.

## 2026-11-17 NAVIGATION & AI (GitHub-only, disk still critical)
- **PR #32 review fix** `aac0786`: `cellsOverlapping` uses checked unsigned spans, refuses oversized ranges before allocating, and leaves the output untouched on failure.
- **Draft stack #33 → #36 (core only, no capability, no Studio or Unreal wiring):**
  - #33 navigation (A*, graph and grid, planner);
  - #34 blackboard and behavior trees;
  - #35 utility decisions;
  - #36 perception data model.
- **Verification:** `clang++ -fsyntax-only` only. Nothing built or run, so **NOT VERIFIED**.
- **Next:** the final phase. Build and run every suite (`ctest`), then the integration review, protocol exposure, Unreal adapters (Navigation System, BT, AI Perception) and end-to-end validation.

## 2026-11-17 OWNER-REQUESTED PAUSE / REMOTE RESUME CHECKPOINT
- **Purpose:** preserve the current work before the coding assistant's token/session limit. No local machine access, downloads, builds, cleanup, or destructive operations were performed for this checkpoint.
- **GitHub snapshot branch:** `checkpoint/titan-2026-11-17-pr36` (created from PR #36 commit `ae123d692391a2dd67bd65b31b1d9954ecb9c1ff`). This branch contains the complete stacked core work through PR #36; this checkpoint documentation is committed only to the snapshot branch and does not alter PR #36.
- **Repo:** `geniusa2009-stack/TITAN` (private). **Unmerged stacked draft PRs:** #16 through #36. Keep them and their bases untouched unless the owner explicitly authorizes changes; no force-push, mass rebase, merge to integration, or deletion.
- **Latest functional branch:** `feature/perception` / draft PR #36, commit `ae123d692391a2dd67bd65b31b1d9954ecb9c1ff`. Earlier system descriptions and restrictions are in the sections above.
- **PR #32 integer-overflow fix:** `aac07860c12e075d7898e065338cba75514e26c3`, carried into the #33-#36 stack.
- **Outstanding code-review item:** PR #34 comment ID `6950134201`: the behavior-tree timeout can advance when the node-visit budget prevents visiting the child. Fix in the appropriate feature branch and propagate safely through #36; add regression coverage. **Not yet fixed as of this checkpoint.**
- **Actual verification:** code authors reported compiler syntax-only checks, but **no full build, unit suite, integration test, CI, Unreal run, FPS benchmark or end-to-end verification** for the recent core systems. The 31 new AI/navigation tests are **NOT VERIFIED**. Do not state the code is production-tested.
- **Disk blocker:** Mac SSD was reported critically low; **do not** perform local builds/tests, launch Unreal, fetch large repositories, download assets, delete files, or trigger billed macOS CI until the owner confirms adequate disk and authorizes it. Unreal validation was deferred until at least ~40 GB free (actual requirements may be higher).
- **Resume protocol:** First read `CLAUDE.md` and this file at `checkpoint/titan-2026-11-17-pr36`; inspect the current GitHub PR stack and recent commits, confirm whether anything newer than #36 exists, and ask for the owner's next objective only if no explicit objective is supplied. Do not redo work already committed; do not begin the deferred full integration review or Unreal adapter work unless the owner requests it.
- **Scope of backup:** GitHub tracks committed files on the remote branches. The assistant cannot independently confirm any uncommitted local working-tree changes or temporary scratchpad content from another session; the owner should have the coding assistant check and push any such changes separately if they exist.

## 2026-11-17 PR #34 REVIEW FIX (GitHub-only)
- **Fixed** on `feature/behavior-trees` (`306f47b`): a timeout no longer counts a tick, or aborts its child, when the node-visit budget ran out before the child was reached. `tick()` now refuses budgets below `BehaviorTree::height()`, so every tick reaches a leaf. A regression test was added.
- **Propagated** by clean merges: `feature/utility-ai` (`e1dd572`) and `feature/perception` (`574fd6b`). The PR #36 tip is now `574fd6b`, so this snapshot branch predates the fix.
- **Verification:** `clang++ -fsyntax-only` on the merged files only. NOT VERIFIED (no build or test run).
- **Open review items:** none known.
