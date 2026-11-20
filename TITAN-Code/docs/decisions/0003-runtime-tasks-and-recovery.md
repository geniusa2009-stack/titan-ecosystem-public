# ADR 0003 — Event-loop runtime, background tasks and crash recovery

Status: accepted (2026-11-09).

## Context
Phase 1 handled requests strictly one after another on one thread. Indexing, search and file
listing blocked editing, and there was no cancellation or protection for unsaved work.

## Decision

**Runtime.** Three kinds of threads:
- A reader thread turns framed input into events. Credit-based backpressure caps it at 64
  unprocessed messages.
- One event loop owns all document state. It applies edits immediately, in request order,
  without locks.
- A writer thread serializes output.

Slow work is submitted to `titan-tasks`: a bounded queue with high/normal/low priority and
anti-starvation, cooperative cancellation tokens with deadlines, and panic isolation.
Completed tasks come back as events. Responses to background requests may arrive out of order.

Not chosen: an async runtime (tokio). The work is CPU-bound parsing and blocking file I/O; a
fixed thread pool with explicit cancellation is simpler to reason about and test. It also
keeps `titan-text` and `titan-doc` free of async.

**Stale results.** Every parse is tagged with the revision it read. The event loop edits the
stored tree with each change (`Tree::edit`) and cancels any parse in flight. A result whose
revision is no longer current is dropped and counted, and a new parse is scheduled from the
continuously edited tree. Only one parse per document is in flight; later edits set a
`needs_parse` flag (coalescing).

**Recovery journal.** This is per document, in a state directory outside the workspace:
- Records are framed with length and CRC-32 and appended with one `write` before the edit is
  acknowledged, so a process crash loses nothing that was acknowledged. `fsync` is batched
  every 200 ms.
- An initial checkpoint (for files up to 4 MiB) keeps the journal recoverable even if the file
  changes externally.
- Compaction rewrites the journal as base + checkpoint once it exceeds 8 MiB.
- Retention: 14 days and 1 GiB for inactive journals.
- Restore puts the recovered text into an unsaved buffer as one transaction. It never writes
  the file, and needs `acceptConflict` when the file changed on disk.

Not chosen: `fsync` on every record. Typing latency would then depend on storage latency;
`benchmarks` measure both paths (`journal_append`, `journal_append_fsync`).

## Limitations
- One client per process.
- Cancellation is cooperative: a task that never checks its token runs to completion, though
  its result is still discarded.
- Index requests answer from the published state. `waitForIdle` makes them wait without
  blocking the loop.
- Journals of very large files (> 4 MiB) depend on the file being unchanged until the first
  compaction.

## Evidence
`crates/titan-tasks` tests (ordering, starvation, rejection, cancellation, deadline, panic,
shutdown); `crates/titan-recovery` tests (truncation, corruption, conflict, compaction,
retention); `crates/titan-server/tests/e2e.rs` (kill/restart recovery, conflict, cancellation,
out-of-order responses, timeouts, notifications); the `titan-probe` recovery phase in CI.

## Replacement criteria
Profiling showing the event loop as the bottleneck (then shard documents across loops), or
a need for multiple concurrent clients sharing documents.
