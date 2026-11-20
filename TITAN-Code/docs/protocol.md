# TITAN headless protocol — version 1

Transport: JSON-RPC 2.0 messages framed with `Content-Length` (the LSP base protocol) on the
`titand` process's stdin/stdout. Diagnostics go to stderr. There is no network listener.

```
Content-Length: 87\r\n
\r\n
{"jsonrpc":"2.0","id":1,"method":"session.hello","params":{"protocolVersion":1}}
```

- Bodies above 64 MiB and header lines above 8 KiB are rejected.
- A body that is not valid JSON gets a `-32700` error with `id: null`; the connection stays open.
- A framing error (bad header, missing `Content-Length`, oversized or truncated body) gets one
  final `-32600` error with `id: null`, then the server closes the stream.
- Requests without `id` are notifications and get no response. Batches are not supported.
- `session.hello` must be the first request; anything else before it fails with `-32009`.
- Requests are read in order. Most are answered immediately. The **background methods**
  (`workspace.search`, `workspace.listFiles`, `recovery.list`, and `index.status` with
  `waitForIdle`) are answered when their work finishes, so **responses can arrive out of
  order**. Match them by `id`.
- `$/cancelRequest {id}` (notification or request) cancels a background request. Its
  response is then error `-32800`. Background requests have a deadline (`requestTimeoutMs` in
  `session.hello`, default 30 s; error `-32014`). A full background queue gives `-32013`. A
  response larger than the limit (64 MiB) becomes error `-32015`.
- When the client disconnects or sends `session.shutdown`, pending requests are answered with
  `-32800`, background work is cancelled, and the server exits.

## Positions

`Position = { line, character }`, zero-based. `character` is in the unit negotiated by
`session.hello`: `"utf-8"` (bytes) or `"utf-16"` (code units, the default, as in LSP).
Lines end at LF, CRLF or a lone CR. `character` may not point past the end of the line's
content (into the terminator), into the middle of a UTF-8 sequence, or into a UTF-16 surrogate
pair. Such positions fail with `-32002`; they are never clamped silently.

`Range = { start: Position, end: Position }`, end exclusive.

## Revisions

Each open document has a `revision` (u64) that starts at 0 and increases by exactly one for
every applied edit, undo and redo. It never decreases. Every mutating request
(`applyEdits`, `undo`, `redo`, `save`) carries `expectedRevision`; if it is not the current
revision the request fails with `-32001` and
`data: { expectedRevision, currentRevision }`. Nothing is applied in that case. Positions
in a request are interpreted against `expectedRevision`.

`dirty` is true when the content differs from the last loaded or saved content. Undoing back
to the saved state makes the document clean again.

## Methods

| Method | Params | Result |
|---|---|---|
| `session.hello` | `protocolVersion`, `clientName?`, `positionEncodings?` (preference order), `capabilities?` (`changeNotifications`, `indexProgress`, `syntaxDiagnostics`, `diskNotifications`, `fileEvents`, `semanticNotifications`), `requestTimeoutMs?` | `protocolVersion`, `serverName`, `serverVersion`, `positionEncoding`, `methods`, `capabilities` (incl. `recovery`, `recoveryDurability`, `backgroundMethods`, `fileWatching`, `languageServers`) |
| `system.metrics` | — | task pool counters, cancelled/timed-out requests, parse counters (incremental, full, stale results dropped), journal counters (incl. retries), `disk` (checks, conflicts, reloads, stale checks), `watch` (watcher and backend counters), `lsp` (state, generation, pid, process metrics), `graph` |
| `session.shutdown` | — | `null`; the server exits after responding |
| `system.health` | — | `status`, `uptimeMs`, `workspaceRoot`, `openDocuments` |
| `workspace.open` | `path` | `root` (canonical). Fails with `-32011` if open documents are dirty |
| `workspace.close` | `discard?` | `null` |
| `workspace.listFiles` | `limit?` (default 10 000, max 100 000) | `files` (relative, `/`-separated, sorted), `truncated` |
| `workspace.search` | `query`, `caseSensitive?` (default true; ASCII folding), `maxResults?` (default 1 000) | `matches[]` (`path`, `range`, `lineText`, `source`: `"disk"`/`"buffer"`, `revision?`), `truncated`, `filesSearched`, `filesSkipped` |
| `document.open` | `path` (relative to root, or absolute inside it) | `documentId`, `path`, `revision`, `dirty`, `lineEnding` (`lf`/`crlf`/`cr`/`mixed`/`unknown`), `hasBom`, `lineCount`, `lenBytes`. Opening an already-open file returns the existing document |
| `document.read` | `documentId` | `documentId`, `revision`, `dirty`, `text` |
| `document.applyEdits` | `documentId`, `expectedRevision`, `edits[]` (`range`, `text`) | `EditResult` |
| `document.undo` / `document.redo` | `documentId`, `expectedRevision` | `EditResult` (`applied: false` if nothing to do) |
| `document.save` | `documentId`, `expectedRevision`, `force?` | `documentId`, `revision`, `bytesWritten`, `dirty`, `strategy` (`exchange`: atomic swap + verification; `rename`: file system without swap support; `createNew`: no-clobber create). `-32005` with `data.kind` (`modified`/`deleted`/`created`/`modifiedDuringSave`) and `data.preservedCopy?`; the buffer is untouched |
| `document.checkDisk` | `documentId` | `documentId`, `changedOnDisk`, `disk` (after a fresh background check; may trigger `document/diskChanged`) |
| `document.resolveConflict` | `documentId`, `expectedRevision`, `resolution` (`keepBuffer`/`reloadFromDisk`), `diskHash?` (the hash the client saw) | `document`, `resolution`, `reloaded`. `keepBuffer`: the next save may replace exactly that disk version. `reloadFromDisk`: one undoable transaction. `-32018` if the disk changed again since `diskHash`; `-32019` if there is no conflict |
| `document.close` | `documentId`, `discard?` | `null`. Fails with `-32011` if dirty and not discarded |
| `index.status` | `waitForIdle?`, `timeoutMs?` (default 10 s) | `evidence`, `generation`, `idle`, `scanning`, `pendingParses`, `filesIndexed`, `symbols`, `filesWithSyntaxErrors`, last scan `reindexed`/`unchanged`/`removed`/`skipped`, `staleResultsDropped`, `incrementalParses`, `fullParses` |
| `index.symbols` | `query` (case-insensitive substring), `maxResults?` (default 200) | `evidence`, `generation`, `indexing`, `matches[]` (`name`, `kind`, `path`, `range` of the name, `containerName?`, `source`: `"disk"`/`"buffer"`, `revision?`) |
| `index.rescan` | — | `{started: true}`; files whose disk version (length + hash) is unchanged are not re-parsed |
| `recovery.list` | — | `enabled`, `journals[]` (`journalId`, `path`, `state`: `recoverable`/`conflict`/`noChanges`/`unrecoverable`, `disk`, `recoveredRevision`, `editRecords`, `truncatedTail`, `corruption`, `reason`, `createdUnixMs`), retention cleanup counts |
| `recovery.restore` | `journalId`, `acceptConflict?` | `document` (opened, dirty, recovered text applied as one undoable transaction), `recoveredRevision`, `conflict`. The file is not written. `-32016` if the file changed and `acceptConflict` is not set; `-32017` if unknown or unrecoverable |
| `recovery.discard` | `journalId` | `{discarded}` |
| `intel.status` | — | `syntax` (index state), `semantic` (language server state: `disabled`/`starting`/`indexing`/`ready`/`restarting`/`failed`/`stopped`, generation, quiescent, pid, crashes, process metrics), `graph`, `limitations` |
| `intel.definition` / `intel.references` / `intel.hover` / `intel.completion` | `documentId`, `expectedRevision` (must be current), `position`, `includeDeclaration?` (references), `timeoutMs?` | `IntelResult` |
| `intel.documentSymbols` | `documentId` | `IntelResult` (semantic + syntax) |
| `intel.workspaceSymbols` | `query`, `maxResults?`, `timeoutMs?` | `IntelResult` (semantic + syntax index) |
| `intel.diagnostics` | `documentId` | `IntelResult` (semantic diagnostics of the last published version + syntax diagnostics of the last parse) |
| `system.memory` | — | `process` (`/proc`: VmRSS, RssAnon/File/Shmem, VmHWM, smaps), `heap` (live bytes, profiling builds only), `estimates` per subsystem (documents, undo, syntax trees, index, graph, journals, language server, tasks), `resources` (memory pressure), `languageServerProcess` |
| `intel.server` | `action`: `stop` / `start` / `restart` | `state` — explicit language-server lifecycle |
| `twin.impact` | `documentId`, `expectedRevision`, `position` | `target`, `known` (definitions, references, callers — rust-analyzer, each with status), `structural` (symbol, module, package, importers, dependents, tests — graph, with provenance), `heuristic.nameMatches`, `affectedFiles` (reasons, open revision, diagnostics), `coverage` (`complete: false`, unknowns) |
| `changes.propose` | `changes[]` (`edit`/`create`/`delete`/`rename` with `expected`: `buffer {revision, hash}` / `disk {hash, len}` / `absent`), `source?`, `preconditions?`, `expectedResult?`, `annotations?` | `proposal`, `status` (`validated`/`rejected`), `validation` (problems, per-file preview with unified diff, patch id) |
| `changes.preview` / `changes.get` / `changes.list` | `proposalId` | read-only re-validation / proposal + evidence / all proposals |
| `changes.approve` | `proposalId`, `by?` | `status: approved` — required before `changes.apply` |
| `changes.apply` | `proposalId` | `outcome` (`applied`/`rolledBack`/`partiallyApplied`/`failed`), transaction report, `evidence`, `evidencePath`. Re-validates first: `-32001` with `data.validation` when stale, nothing written. Open documents get one undoable transaction each (not saved); disk files a journaled transaction |
| `changes.verify` | `proposalId`, `checks[]` (`{"kind": "cargo_check" / "cargo_test" / "cargo_build" / "cargo_fmt_check" / "cargo_clippy", ...}`), `policy?` (`allow_build_scripts`, `allow_proc_macros`, `allow_network`; all false by default), `timeoutMs?` | baseline (git HEAD) vs candidate (HEAD + proposal) in isolated worktrees: runs, test counts, verdicts; recorded in the evidence (not-run checks recorded as not run) |
| `changes.restore` | `proposalId` | puts back the disk files (only if each still holds the applied content) and undoes the buffer transactions (only if not edited since) |
| `refactor.rename` | `documentId`, `expectedRevision`, `position`, `newName` | a Verified Changes proposal built from rust-analyzer's `prepareRename` + `rename` WorkspaceEdit (versioned buffer edits, disk edits, file operations, annotations) |
| `graph.summary` | — | `generation`, `state` (`none`/`building`/`ready`/`rebuilding`), `stale`, node/edge counts, packages, stats (unreachable files, unresolved modules/imports, external imports), `limitations` |
| `graph.node` | `id` | `node`, `outgoing[]`, `incoming[]` (each edge with `evidence {kind, path, line}`) |
| `graph.query` | `relation` (`children`/`parents`/`defines`/`definedIn`/`imports`/`importedBy`/`tests`/`testsOf`/`dependsOn`/`dependents`, or `find` with `id` = a name or `path:<file>`), `id` (numeric node id), `maxResults?` | `results[]` (nodes with the edge that links them), `truncated` |

`document.open` also returns `pendingRecovery`: ids of journals that earlier sessions left
for this file.

`document.open` (and every `document` in results) also carries `disk` (`inSync`/`modified`/
`deleted`/`unreadable`) and `recovery` (`disabled`/`clean`/`journaling`/`failed`).

`IntelResult = { documentId, currentRevision, results[] }`; each result is one source's
`Evidence = { source, kind (semantic/syntax/nameMatch), status (current/stale/unavailable/
failed/timeout), documentRevision, generation, complete?, limitations[], error?, elapsedMs,
items }`. Sources are never merged. `stale` means the document changed (or the language
server restarted) while the answer was computed; positions then refer to
`documentRevision`. Locations carry `path` (workspace-relative, or absolute with
`external: true`), `range`, and `revision` when they point into an open document.

`EditResult = { documentId, applied, previousRevision, revision, dirty, changes[] }`. `changes`
are the applied edits, sorted, with ranges in coordinates of `previousRevision`, so a client
can replay them on its own copy.

`edits` in one request must not overlap; they are applied atomically (all or nothing).
Several insertions at the same position are applied in request order.

Search covers files that are not ignored (`.gitignore`, `.ignore`, `.git/info/exclude`,
hidden entries). Open documents are searched from memory (`source: "buffer"`, with the
revision searched); other files from disk. Binary, non-UTF-8 and files over 8 MiB are
skipped and counted in `filesSkipped`.

## Notifications (server → client, opt-in via `session.hello` capabilities)

| Method | Params |
|---|---|
| `document/didChange` | `documentId`, `kind` (`edit`/`undo`/`redo`), `previousRevision`, `revision`, `dirty`, `changes[]` (ranges in `previousRevision` coordinates) |
| `document/syntaxDiagnostics` | `documentId`, `revision`, `evidence`, `diagnostics[]` (`range`, `severity`, `message`); only sent for the current revision |
| `index/progress` | `phase` (`scanning`/`indexing`/`idle`), `filesDone`, `filesTotal`, `pendingParses`, `generation` |
| `document/diskChanged` | `documentId`, `path`, `kind` (`modified`/`deleted`/`restored`), `action` (`reloaded`: clean buffer replaced; `conflict`: dirty buffer kept; `none`), `dirty`, `revision`, `diskHash?`, `diskLen?`, `cause` (`watcher`/`checkDisk`/`save`/`rescan`/`resolveConflict`) |
| `workspace/filesChanged` | `seq`, `rescan` (events were lost; the server rescanned), `changes[]` (`path`, `kind`: `created`/`modified`/`removed`/`renamed`, `from?`, `isDir?`) |
| `recovery/degraded` | `documentId`, `reason`, `retrying` — journaling failed (e.g. disk full); editing continues unprotected until a later retry succeeds |
| `document/semanticDiagnostics` | `documentId`, `source`, `revision` (the version the server analysed), `receivedAtRevision`, `current`, `generation`, `diagnostics[]` |
| `document/lateWrite` | `documentId`, `path`, `preservedCopy`, `bytes`, `detectedAfterMs` — a program wrote to the version a save replaced (through a descriptor opened before the save); the bytes were preserved (ADR 0010) |
| `system/resourcePressure` | memory pressure level and what was shed (ADR 0012) |
| `intel/state` | `server`, `state`, `generation`, `quiescent`, `crashes`, `reason` |

## Recovery

Started with `--state-dir PATH` (or `$TITAN_STATE_DIR`, or the per-user default; disabled with
`--no-recovery`). From the first unsaved change, every committed change of a document is
appended to a journal before the response is sent. The journal is deleted on save or on a
discarding close. It is kept when the process crashes or shuts down with unsaved work.

Durability (`recoveryDurability` in `session.hello`): acknowledged edits survive a crash or
kill of `titand`; after an OS crash or power loss up to 200 ms of the newest edits can be
lost (batched `fsync`). If journaling fails (disk full), the client gets `recovery/degraded`,
editing continues, and a fresh fully checkpointed journal is attempted on the first change
5 s later.

## File watching and disk reconciliation

`titand` watches the workspace (`--no-watch` disables; `.git`, `target`, `node_modules` and
editor temp files are ignored). For open documents every event leads to a background read +
BLAKE3 comparison: the same content → nothing; changed and the buffer clean → reloaded
(`document/diskChanged`, `action: reloaded`, undoable); changed or deleted and the buffer
dirty → kept, `action: conflict`, saves refused until `document.resolveConflict`. Lost
events (`rescan`) and directory events trigger an incremental rescan plus a check of every
open document. Rust files that are not open are re-indexed.

## Language server

`--rust-analyzer PATH` (optional) starts rust-analyzer for the workspace under a supervisor
(restart with backoff, at most 3 crashes per minute, then `failed`). Open Rust documents are
synchronised from their **buffers** (`version` = TITAN revision). Without a language server,
or while it is not ready, `intel.*` answers contain an `unavailable` semantic entry with the
reason, next to the syntax evidence. Default settings disable `cargo check`, build scripts
and proc macros (reported as a limitation); `--lsp-settings JSON` replaces them.

## Index

Indexing runs in the background: a workspace scan starts when the workspace opens; open Rust
documents are re-parsed incrementally after each change (the previous tree is edited and
reused), and parses of outdated revisions are cancelled or discarded. `index.*` methods answer
from the published state. Disk files (not ignored, at most 4 MiB) are indexed by version
(length + content hash); open documents by buffer revision. Every result carries `evidence: "syntax:tree-sitter-rust"`: definitions and nesting
from a parse tree only, with no name resolution, types or references. Symbol lines are counted
by LF.

## Error codes

| Code | Meaning |
|---|---|
| -32700 | Body is not valid JSON |
| -32600 | Invalid request envelope, or framing error (connection then closed) |
| -32601 | Unknown method |
| -32602 | Invalid params |
| -32603 | Internal error |
| -32001 | Stale revision (`data.expectedRevision`, `data.currentRevision`) |
| -32002 | Invalid position or range, or overlapping edits |
| -32003 | Unknown document id |
| -32004 | Path resolves outside the workspace (including through symlinks) |
| -32005 | File changed on disk since load/save; retry with `force: true` to overwrite |
| -32006 | Binary, non-UTF-8 or UTF-16 file |
| -32007 | File above the size limit (64 MiB) |
| -32008 | No workspace open |
| -32009 | `session.hello` not sent yet |
| -32010 | Filesystem error (missing file, permission denied, ...) |
| -32011 | Unsaved changes would be discarded |
| -32012 | Unsupported protocol version (`data.supported`) |
| -32013 | Background queue full; retry later |
| -32014 | Background request exceeded its deadline |
| -32015 | Response exceeds the size limit |
| -32016 | Restoring would discard newer content on disk (`acceptConflict` required) |
| -32017 | Recovery unavailable (disabled, unknown id, or unrecoverable journal) |
| -32018 | The disk changed again since the version the client resolved against |
| -32019 | No disk conflict to resolve |
| -32020 | The project graph has not been built yet |
| -32021 | The operation needs the language server, which is not available |
| -32800 | Request cancelled (or server shutting down) |

Codes are stable: a code is never reused for a different meaning.

## Not in version 1

Unix-socket transport, multiple concurrent clients, language servers other than
rust-analyzer, rename/code actions/formatting through the language server. See
`docs/STATUS.md`.
