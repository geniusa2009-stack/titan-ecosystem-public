# ADR 0008 — Verified changes: proposals, validation, journaled application, evidence

Status: accepted (2026-11-10). Crate: `crates/titan-changes`.

## Context
Phase 4 needs one way to make multi-file changes that works the same for a developer
command, an LSP refactoring (for example a rust-analyzer rename `WorkspaceEdit` with create,
rename and delete operations and change annotations) and, later, agents. Each change must
say what it was computed against, be checked before it touches anything, be previewable,
be applied without overwriting concurrent work, be undoable, and leave a record of what
happened. No AI provider, network or language server may be required.

## Decision
**Model.** A `Proposal` (id, source kind + detail, base snapshot, `FileChange`s,
human-readable preconditions and expected result, change annotations, status) serialised
as camelCase JSON. Each `FileChange` names its base explicitly: `ExpectedBase::Buffer
{ revision, hash }` for an open editor buffer, `Disk { hash, len }` for a file,
`Absent` for nothing. Edits are byte ranges into that base. Hashes are BLAKE3
(`titan_doc::ContentHash`, ADR 0006). `patch_id` is BLAKE3 over a versioned canonical
JSON of the changes with normalised paths and lowercase hashes, so it is independent of
proposal id, time, source and status.

**Validation** (`validate(&Proposal, &dyn WorkspaceView)`) reads, never writes. The caller's
`WorkspaceView` decides which paths are open buffers; `DiskView` is disk-only. Checked:
paths (relative, no `..`, no symlink leaving the root, no symlink as the final
component), bases (stale revision/hash/length or missing file → `stale`), edits (in bounds,
on UTF-8 boundaries, sorted, non-overlapping; several insertions at one offset allowed, in
order), create/rename targets absent, no path touched twice or nested in another changed
path, buffer/disk mismatches in both directions (`bufferIsOpen` protects unsaved edits;
`bufferNotOpen`), annotations declared. Annotations with `needsConfirmation` are reported;
`apply` refuses them unless the status is `approved`. Each change gets a `FilePreview` with
before/after hashes and a unified diff (Myers O(ND), 3 lines of context, GNU/BSD
conventions for ranges and `\ No newline at end of file`). Above 1,000 changed lines or
200,000 lines in total the preview is a "file replaced" summary with hashes.

**Application** (`apply`) is a journaled transaction under the caller's state directory,
`changes/txn-<id>/`:
1. Prepare: validate again; compute every new content; copy every original
   (`before/<n>`) and every proposed content (`after/<n>`) into the transaction directory,
   `fsync` files and directories; write the plan as the first record of `journal`
   (CRC-32-framed JSON lines, `fsync`ed per record). Nothing in the workspace is touched
   before this point.
2. Commit, in change order (a rename is "create target" then "delete source"): journal
   `begin n`; write with `titan_doc::replace_file` against the validated content
   (`Expected::Content`) or `Expected::Absent` for creates (no-clobber); deletes move the
   file aside atomically, hash it, and remove it only if it is exactly the expected content
   (otherwise put it back with a no-clobber link); journal `applied n` or `notApplied n`.
3. On failure at operation k: journal `rollbackStarted`, then undo k..0 in reverse,
   deciding **by content on disk**: our bytes → restore the original (again through
   `replace_file`, expecting our hash); the original → nothing; anything else → leave it
   and report a `rollbackConflict` naming the disk file and both copies in the transaction
   directory. Terminal record `rolledBack` or `rollbackIncomplete`.
4. `recover(state_dir)` finishes every transaction whose journal has no terminal record
   with the same undo rules. A torn final record is ignored; a damaged record in the middle
   makes recovery report the transaction as unreadable and touch nothing.
5. `restore(state_dir, &EvidenceRecord)` undoes a committed transaction, only if every file
   still holds exactly the applied content; otherwise it refuses and names each changed file.
6. Test hooks (`apply_with_hook`, `restore_with_hook`) can fail or "crash" (stop without
   further writes) before/after each operation and before each undo, and observe the
   `replace_file` phases.

**Buffers** are never mutated by this crate: changes to open buffers come back as
`BufferEdits { path, expectedRevision, edits }` for the server to apply as undoable
transactions; only disk targets join the transaction, the rollback and `restore`.

**Evidence.** `EvidenceRecord` (proposal and patch id, source, base, affected files with
before/after hashes and target, validation summary, checks, approval, outcome, restore
information, notes), one JSON file per proposal at `changes/evidence/<id>.json`, written
with temp + `fsync` + rename + directory `fsync`. Check status is `passed`, `failed`,
`notRun { reason }`, `timedOut` or `error { message }`: what a command reported when it ran.
A check that did not run is recorded as `notRun` with its reason. A passing check is
evidence about that run, not a statement that the change is correct.

## Limits (not solved; documented)
- **Not atomic.** Other processes can observe a state where some files are written and
  others are not, between the first write and the commit or rollback. A crash leaves that
  state until `recover` runs (call it at startup, never concurrently with `apply` on the
  same state directory; there is no lock).
- The OS-level limits of ADR 0006 apply per file (writers holding an old descriptor, hard
  links, the `rename` fallback window where the swap is unsupported).
- Undo decides by content: if another writer produced exactly our bytes, undo treats them
  as ours. If a file someone else created has exactly our proposed content in the moment a
  recovery runs for a create that was begun but not confirmed, it is removed; its bytes stay
  in `after/<n>`.
- Created files get the process default mode (umask); permissions are set after the
  no-clobber create, a short window in which the file has mode 0600. Renamed files keep
  their mode; deleted-then-restored files get their recorded mode back. Ownership, extended
  attributes and timestamps are not preserved.
- Case-insensitive file systems: two spellings of one path are not detected as the same
  path by validation (the replace itself is still conflict-checked).
- Offsets for disk bases include a UTF-8 BOM if the file has one; buffer offsets index the
  buffer text (titan-doc keeps the BOM outside the text).
- The transaction directory keeps copies of all original and proposed bytes; nothing
  prunes it yet.
- Diff previews above the caps are summaries only.

## Alternatives
- Apply each file with `Document::save` only (no journal): no rollback after a partial
  failure, no crash recovery.
- Stage the whole tree in a copy and swap directories: not possible for a workspace that
  other tools have open; not atomic across mount points either.
- Git stash / worktree as the undo mechanism: depends on git and on the workspace being a
  repository; does not protect untracked files or open buffers.
- An external diff library: the line diff is ~150 lines, is tested against `diff -u`
  output, and avoids a dependency (dependency policy).

## Evidence
`crates/titan-changes/tests/validate.rs` (every problem kind, previews incl. CRLF, missing
final newline, Unicode, insertions; patch-id stability; JSON shape),
`tests/apply.rs` (multi-file success with exact bytes and modes; failure on the 3rd of 5
files; failure right after a write; external change during rollback; crash after two
applied files + `recover`; crash during rollback; torn and damaged journals; concurrent
create; change inside the save window; delete and rename verification; restore and refused
restore; evidence round trip; buffer targets), `tests/random.rs` (150 fixed seeds of random
proposals with random failures and crashes, asserting the byte-preservation invariant),
`src/diff.rs` unit tests (goldens checked against `diff -u`, and a randomized minimality
check against an LCS table). All run on Linux and macOS in CI.

## Replacement criteria
A transactional file-system primitive available on the supported platforms, or evidence
(benchmarks on real refactorings) that staging copies of every file costs more than the
latency budgets allow.
