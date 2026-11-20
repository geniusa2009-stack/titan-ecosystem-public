# ADR 0010 — Writers holding the old file open: detection, preservation, guarantee boundary

Status: accepted (2026-11-10). Extends ADR 0006.

## Context
ADR 0006 closed the check-then-replace window with an atomic swap, but left one gap: a
program that opened the file *before* TITAN saved it and keeps writing through that
descriptor afterwards writes into the replaced inode. Atomic-rename editors lose those
bytes silently.

## Decision
- After a successful replacement (swap or rename), the replaced version is **unlinked but
  kept open** (`titan_doc::DisplacedFile`). It is invisible in the directory.
- `titand` checks it on every tick with `fstat` (size, mtime, ctime) for a grace period of
  **30 s**; when it changed — and once more by content when the grace period ends or the
  server shuts down — it reads the old version through the descriptor and compares it with
  what was replaced (BLAKE3). Late bytes are written to
  `<state-dir>/displaced/<key>-<name>.<ms>.late` (or next to the file as
  `<name>.titan-latewrite-<ms>` without a state directory) and reported with
  `document/lateWrite { documentId, path, preservedCopy, bytes, detectedAfterMs }`.
  Each late write is reported once; watching continues for further writes.
- At most 64 replaced versions are watched at once (the oldest is checked and released).

## Guarantee boundary (what is and is not promised)
| Situation | Outcome |
|---|---|
| Change by path at any moment before the swap (in place, truncation, append, atomic replace) | detected; the swap is undone or the save refused; nothing lost (ADR 0006) |
| Write through an old descriptor within 30 s after the save | detected, preserved, reported |
| Write through an old descriptor later than 30 s after the save | **not seen**: TITAN has closed its descriptor; the writer still writes into an unlinked inode |
| `mmap` writes to the old version | seen only if they update mtime (msync) within the window |
| Change by path after the save | it changes the *new* file; the watcher/`checkDisk` reconciliation handles it (ADR 0005) |
| `titand` killed during a save | the target is the old or the new version, never partial (rename/swap are atomic); a `.titan-save-*` temp file may remain; unsaved edits are in the recovery journal |
| Directory renamed during a save | the save fails; the buffer is kept; nothing written in either location |

No portable primitive lets an editor coordinate with arbitrary writers; TITAN does not
claim universal race freedom.

## Evidence
`crates/titan-doc/src/tests.rs` (`external_processes`: real `sh` processes appending and
overwriting through descriptors opened before the save, truncation/append by path,
directory renamed mid-save, three concurrent atomic-replace writer processes);
`crates/titan-server/tests/disk.rs` (`late_write_through_an_old_descriptor_is_preserved_and_reported`,
`killed_during_saves_leaves_a_complete_file_and_a_recoverable_journal`).
