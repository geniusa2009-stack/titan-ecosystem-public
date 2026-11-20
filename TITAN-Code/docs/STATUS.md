# Implementation status

Every entry says how it is verified. "CI" means GitHub Actions (`rustc 1.91.0`): Linux
(ubuntu-latest, 2 vCPU) is the primary environment; a macOS job runs the whole workspace.
Nothing is built locally; see `CLAUDE.md`.

Phase 4 branch: `feat/phase4-project-twin` (stacked on Phase 3, PR #4).
Fully green run: `9b1e23e`, CI run 42752890165 — rustfmt, clippy `-D warnings`, Linux tests +
release build + probe, macOS workspace tests + probe, cargo deny, benchmarks with latency
budgets. `aa8a990` (graph-update coalescing) passed every job except rustfmt in run 42754408384;
the rustfmt fix and these docs are in the final commit (its run is linked in the PR).

Tests (run 42752890165, `--include-ignored`): Linux **275 passed, 0 failed, 0 ignored**
(Phase 3: 191; the log shows 286 because 11 watcher/semantic e2e tests run a second time
with `--nocapture` for evidence). macOS **271 passed, 0 failed** — the same suite minus the 3
storage-exhaustion tests that need the Linux tmpfs (skipped by name, reported as filtered)
and one Linux-only `/proc` test — plus the 29 `titan-watch` tests again with output.

## Phase 4 — implemented and tested

| Area | What works | Evidence |
|---|---|---|
| Late writers (ADR 0010) | The replaced version stays open (unlinked) after a save; writes through descriptors opened before the save are detected (fstat, then content) for 30 s, preserved in `<state>/displaced/`, reported once with `document/lateWrite` | `titan-doc` tests with real `sh` processes: late append, same-length overwrite through an old fd, truncation/append by path, directory renamed mid-save, 3 concurrent atomic-replace writer processes; e2e late write via titand; titand killed at random points during repeated 2.2 MB saves → always one complete version, journal recoverable |
| Memory profiling | `system.memory`: /proc process memory, live heap (counting-allocator build `--features alloc-stats`), per-subsystem estimates; memory per session phase and rust-analyzer RSS separately in the bench | `benchmarks/results/2026-11-10-phase4-ci.json` → `profile` |
| Resource-aware execution (ADR 0012) | memory pressure from meminfo / cgroup v2: `high` pauses graph rebuilds and drops facts of closed files, `critical` releases the graph; editing/journals/open-document parsing/LSP never shed; graph epochs (no late results after close); `--no-graph`, `--graph-max-files`, `--workers`; `intel.server stop/start/restart`; coalesced graph updates | e2e: simulated critical pressure pauses the graph while editing continues; LSP stopped (process gone) and started again |
| Cold start | per-generation timeline (initialized, ready, first diagnostics, first answer per method) in `intel.status`; cache priming on by default (measured trade-off) | bench `coldStartPrimingOff/On` |
| Project Twin V1 (ADR 0011) | stable opaque ids from semantic keys (survive rebuilds, line shifts, `x.rs`↔`x/mod.rs`, reported file and symbol renames); packages, dependencies, modules, files, functions, types, symbols, tests; contains/defines/imports/tests/dependsOn with provenance (source, analysis, evidence, file, line, version, generation) and freshness at query time; in-place updates when module declarations and `use` paths are unchanged; compact storage | 6 graph unit tests incl. randomized incremental == full rebuild (12 seeds × 30 edits) and identity stability; e2e build/query/rebuild with stable ids |
| Change impact (`twin.impact`) | known (rust-analyzer definition, references, callers via call hierarchy), structural (module, package, importers, dependents, tests), heuristic (name matches), affected files with reasons and diagnostics, coverage + unknowns (`complete: false`) | real rust-analyzer e2e: definition `src/other.rs`, references incl. `src/lib.rs`, caller `entry`, module `other`, package `fixture` |
| Verified Changes (ADR 0008, `titan-changes`) | proposals (edit/create/delete/rename with buffer/disk/absent expectations), validation (paths confined, revisions/hashes, overlaps, boundaries, no clobber, unsaved buffers protected), unified-diff previews, patch ids, explicit approval, journaled multi-file apply with rollback and crash recovery, restore, evidence records (checks passed/failed/not run/timed out) | 41 crate tests incl. 150-seed randomized fault injection; e2e developer proposal (buffer + disk + create → apply → restore) |
| Rename through rust-analyzer | `refactor.rename` → `prepareRename` + `rename` → versioned WorkspaceEdit → proposal; stale proposals rejected on apply; buffer edits as undoable transactions; evidence; restore | real rust-analyzer e2e (all 14 steps of the brief): preview diffs (`-pub fn helper` / `+pub fn add_one`), concurrent modification → `-32001`, nothing written, fresh proposal, `cargo test` on base and candidate in worktrees (passed), approval required, apply, definition current at the new name, evidence on disk, restore to the exact previous bytes |
| Verification pipeline (ADR 0009, `titan-verify`) | typed check catalogue (no shell), trust policy (no build scripts/proc macros/network by default), scrubbed env, per-run target dir, process-group timeout kill, bounded logs, libtest counts, base vs candidate verdicts, worktrees always removed | 9 integration + 21 unit tests on Linux and macOS |

## Measured on CI (run 42754408384, commit `aa8a990`; 150 × 60 KB + 1 MB Rust workspace; single run)
- **titand memory, Phase 3 vs Phase 4 on the same runner** (load scenario with rust-analyzer):
  RSS 356 → 239 MiB, peak 371 → 247 MiB; CPU 13.1 → 9.8 s. rust-analyzer ≈ 1.06 GiB in both
  (separate process, measured separately).
- **Memory by phase** (live Rust heap of the profiling build): index 31 MiB; graph +126 MiB
  (≈ 82 000 function nodes; +153 MiB before compaction); 1 MB document + 2 500 edits +15 MiB;
  **65 MiB remain live after `workspace.close` — not yet attributed** (see limitations).
- **Cold start** (cache priming off / on — on is the default): ready 1.7 s / 8.6 s; first
  definition 8.4 s / 24 ms; first hover 63 ms / 189 ms; warm definition median 3.7 ms / 3.7 ms;
  first definition after 500 edits 2.6 s / 2.2 s. (The Phase 3 "20.4 s cold query" was a first
  query right after an edit burst on a 1 MB file with priming off.)
- **Project Twin**: workspace open → graph ready 7.4 s (cold parse of 152 files on a shared
  pool); full build 3.8 s; structural rebuild from cached facts 339 ms; coalesced in-place
  update of the 1 MB file ≈ 1.5 ms (at most every 250 ms per file).
- **twin.impact** (rust-analyzer): first 3.0 s, then ≈ 47 ms. **Rename preview** 24 ms.
- **50-file Verified Change**: propose + preview 12 ms, apply 200 ms, restore 170 ms.
- **Editing latency under load**: all budgets passed; vs Phase 3 on the same runner all 69
  shared scenarios at 0.30–1.13× (no warning). Editing while rust-analyzer analyses every
  change: 1.99 ms median vs 1.77 ms (1.13×); before coalescing graph updates it was 2.48×
  in run 42753522621 — found by this comparison and fixed in `aa8a990`.

## Known limitations
- 65 MiB of live heap after `workspace.close` is unexplained (no allocation-site profiler in
  CI yet); glibc keeps freed memory resident (RSS 143 MiB after close).
- The graph is still the largest `titand` heap owner on large workspaces.
- Multi-file application is journaled and recoverable, not atomic. Buffer edits are applied
  in memory and not saved.
- Verification is not a sandbox (trust policy + worktree + scrubbed env + timeout).
- Late writes through old descriptors after the 30 s window are not seen.
- Rust only; rust-analyzer only; one language server per workspace. Symbol identities are
  carried over only for renames invoked on the definition.
- Earlier limitations: ADRs 0004–0012 and `docs/audit/2026-11-phase2-audit.md`.
