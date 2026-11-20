# ADR 0006 — Content hashes and conflict-safe saves

Status: accepted (2026-11-10).

## Context
Phase 2 decided "did the file change on disk?" with a 64-bit FNV-1a hash and saved with
check-then-rename (audit finding L1: another writer could change the file between the
check and the rename, and that change was lost). FNV-1a collisions are rare by accident but
cheap to construct, and a collision would turn "someone else's newer file" into "unchanged".

## Decision
**Hashes, by job:**

| Job | Hash | Why |
|---|---|---|
| Is the disk still the version this buffer is based on? (save conflicts, watcher reconciliation, recovery journal base) | BLAKE3-256 (`titan_doc::ContentHash`) | collision-resistant; faster than byte-at-a-time FNV-1a on the CI runner (see `benchmarks/`, `hash_blake3` vs `hash_fnv1a`) |
| Journal record corruption | CRC-32 | detects torn/bit-flipped writes; integrity of a single record, not identity |
| Journal file-name keys, index cache keys | FNV-1a 64 (`stable_hash`) | lookups only; every candidate is verified afterwards |
| Security identity | none | nothing in TITAN authenticates content |

Hashes are computed on load, save, disk checks and file-system events, never per keystroke.

**Saves:** validate (open, fstat, stream + BLAKE3, fstat again) → write temp + fsync →
re-check the `stat` stamp → **atomic swap** (`renameat2(RENAME_EXCHANGE)` on Linux,
`renameatx_np(RENAME_SWAP)` on macOS) → hash the displaced file. If it is not the validated
version, the files are swapped back and the save reports `modifiedDuringSave`; if the swap
back cannot be confirmed, the displaced bytes are kept as `<name>.titan-conflict-<ms>` and
the path is reported. The swap never unlinks anything, so a change made at any moment
before the swap is preserved and detected. Files expected to be absent are created with a
no-clobber rename. Where the swap is not supported (`EINVAL`/`ENOTSUP`/`ENOSYS`/`EXDEV`, or
other platforms) the save falls back to rename and reports `strategy: rename`.

## Limits (not solved; documented)
- A process that keeps an old file descriptor open and writes after the swap writes into
  the displaced inode; those bytes are lost with it. No portable primitive prevents that.
- After a confirmed swap the old version is deleted; another hard link keeps old content.
- The fallback `rename` path keeps a window of microseconds between the last check and the
  rename.
- Content equality is the criterion: a rewrite with identical bytes is not a conflict.

## Alternatives
- Lock files / advisory locks: other editors ignore them.
- `O_TMPFILE` + `linkat`: does not compare-and-replace.
- Keep FNV-1a and add size/mtime: timestamps are coarse and settable.

## Evidence
`crates/titan-doc/src/tests.rs`: modification between validation and write
(`change_after_validation_is_caught_by_the_final_check`), in-place and rename-replacement
immediately before the swap (`change_right_before_replacement_is_swapped_back`), no-clobber
create, 300-save stress against a continuous external writer; BLAKE3 reference vectors.
`crates/titan-server/tests/disk.rs` and `tests/watch.rs` end to end.

## Replacement criteria
A platform primitive that compares and replaces in one call (none exists portably), or
evidence that the extra read before the swap costs more than budgets allow.
