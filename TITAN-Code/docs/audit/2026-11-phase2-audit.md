# Phase 2 code audit (2026-11-09)

Scope: every crate on `feat/core-foundation` at `24b6e5e`, plus CI coverage. Each finding
names its status and the commit that addressed it (branch `feat/phase2-production-core`).

## Critical

| ID | Finding | Status |
|---|---|---|
| C1 | `titan-index` computed every symbol position by rescanning the file prefix (`before.matches('\n')`), so indexing was O(file size × symbols), quadratic per file. | Fixed in `13bf27f` (line table), then replaced by buffer-based positions in `9686a42`. |

## High

| ID | Finding | Status |
|---|---|---|
| H1 | Undo/redo history was unbounded: a long session grew memory without limit. | Fixed `13bf27f`: `HistoryLimits` (entries + bytes); the oldest entries are dropped and the newest is kept. Tested. |
| H2 | `Document::open` checked the size with `metadata` and then read the path, so a file growing in between could be read past the limit. | Fixed `13bf27f`: size checked on the open handle and the read capped at `limit + 1`. |
| H3 | Disk fingerprints used `DefaultHasher`, which is not stable across builds, so it could not identify content persisted across process restarts. | Fixed `13bf27f`: FNV-1a `stable_hash`, with reference-vector test. |
| H4 | `index.*` ran a full refresh on the request thread and re-read and hashed every `.rs` file on each call. A large workspace blocked every other request. | Fixed `9d3b551`: background scan task; per-file version check (length + hash); `index.status` reports state without refreshing. |
| H5 | No protection for unsaved edits: a crash lost all work since the last save. | Fixed `f6a224e` + `9d3b551`: recovery journal, restore/discard protocol, kill/restart tests. |

## Medium

| ID | Finding | Status |
|---|---|---|
| M1 | The header reader accepted unlimited header lines; a stream of short headers kept it looping. | Fixed `13bf27f`: at most 32 header lines. |
| M2 | `"id": null` was treated as a notification and got no response. | Fixed `13bf27f`. |
| M3 | `workspace.search` copied the full text of every open document on the request thread. | Fixed `9d3b551`: O(1) snapshots on the loop; text materialised on the worker. |
| M4 | Search and file listing ran on the request thread, with no cancellation or deadline. | Fixed `9d3b551`: background tasks, `$/cancelRequest`, per-request deadlines. |
| M5 | No response size limit. | Fixed `9d3b551`: `-32015` when a response exceeds `max_response_bytes`. |

## Low / open

| ID | Finding | Status |
|---|---|---|
| L1 | `Document::save` checks for external modification and then renames; another writer can change the file between the two. | Open. The window is microseconds; closing it needs OS-specific compare-and-swap that is not available portably. |
| L2 | `changed_on_disk` hashes the whole file on each call. | Open; called only on save and `document.checkDisk`. |
| L3 | Tree-sitter counts rows by LF only; `TextBuffer` also breaks lines on lone CR. | Mitigated: protocol positions come from the buffer. Tree-internal rows can differ on lone-CR files (byte offsets are exact). |
| L4 | Grapheme queries are O(line length). | Open; documented. |
| L5 | Search folds case for ASCII only. | Open; documented. |
| L6 | Journal `fsync` is batched every 200 ms; an OS crash or power loss can lose the most recent edits. A process crash loses nothing that was acknowledged. | By design; documented in `titan-recovery`. |
| L7 | A rescan re-parses the disk version of files that are open in buffers, then discards the result. | Open; wasted work only. |
| L8 | No file watching: external changes are seen only on save, `document.checkDisk` or `index.rescan`. | Open. Next milestone. |
| L9 | CI runs on Linux only; there is no macOS job and no `cargo deny`. | Open. |

## Research cross-check

Four claims from `research/` were re-checked by hand against the pinned upstream commits and
all matched the source:
- VS Code `pieceTreeBase.ts` lines 1221, 1230 and 1474: line starts are concatenated on every insert.
- Helix `ChangeSet::map` is `unimplemented!()` (`transaction.rs:306-308`).
- Helix `from_changes` only uses `debug_assert!` for ordering.
- Lapce `dispatch.rs:168-179` ignores the result of `buffer.update` and forwards the delta anyway.

The other research claims remain preliminary.

## Phase 3 follow-up (branch `feat/phase3-semantic-intelligence`)

| ID | Status after Phase 3 |
|---|---|
| L1 | Addressed on Linux and macOS: saves swap atomically and verify the displaced file, swapping back on mismatch (ADR 0006). Residual: writers holding an old file descriptor; the `rename` fallback on file systems without swap support (reported as `strategy: rename`). |
| L2 | Fixed: `document.checkDisk` and watcher-triggered checks read and hash on the task pool; only saves hash on the loop (they must be ordered with edits). |
| L6 | Unchanged by design, now advertised: `session.hello` → `recoveryDurability {processCrash: true, powerLossWindowMs: 200}`. |
| L8 | Fixed: `titan-watch` + reconciliation (dirty buffers never reloaded; conflicts reported and resolved explicitly). |
| L9 | Fixed: macOS CI job for the whole workspace; `cargo deny` job. |

New findings while building Phase 3:

| Finding | Status |
|---|---|
| A recovery journal whose creation failed on a full disk stayed behind without a header (found by the tiny-tmpfs e2e test). | Fixed: creation removes the file on any write/sync error; unit + e2e tests on a real 256 KiB tmpfs. |
| A failed journal append could leave a torn record that hid later records. | Fixed: the partial record is truncated away; journaling is retried with a fresh checkpointed journal after 5 s and the client is told (`recovery/degraded`). |
| A journal created when the buffer was already dirty could replay edits onto the wrong base for files above the 4 MiB checkpoint limit. | Fixed: such journals always checkpoint. |
| FNV-1a decided save conflicts (collisions constructible). | Fixed: BLAKE3 for all integrity decisions (ADR 0006); FNV-1a kept only for lookup keys. |
