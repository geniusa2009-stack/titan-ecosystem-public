# State model (State Fabric, first version)

These identities are distinct and must never be confused:

| Identity | Where | Meaning |
|---|---|---|
| Document id | `titan-server` session | Handle for one open file in one session. Not stable across sessions. |
| Buffer revision | `TextDocument::revision` | Count of committed edits/undos/redos. Monotonic; never reused. |
| Content id | `TextDocument::content_id` | Identity of a content state; undo/redo restore earlier ids. Used for dirty tracking. |
| Persisted state | `titan_doc::DiskState` | Length, mtime and content hash of the file as last loaded or saved. |
| Workspace root | `titan_workspace::Workspace` | Canonical root directory; all paths are checked against it. |
| Git commit | — | Not tracked yet. |
| Index generation | — | Not implemented yet (planned with `titan-index`). |
| Agent proposal revision | — | Not implemented yet. |

## Rules in force today
1. Every mutation names the buffer revision it was computed against; a mismatch is rejected
   with `StaleRevision` and nothing changes. Positions are interpreted against that revision.
2. A save names the revision being saved and is refused if the persisted state no longer
   matches the disk (external modification), unless forced.
3. Results derived from a document state say which state they describe: edit results carry
   `previousRevision`/`revision` and ranges in `previousRevision` coordinates; search matches
   from open documents carry the buffer revision they were found in.
4. Snapshots (`TextSnapshot`) are O(1), immutable and tagged with their revision, so a
   background consumer can work on revision R while edits continue, and can tell its result
   is stale by comparing R with the current revision.

## Concurrency today
`titand` processes one request at a time per connection, so there is exactly one writer per
document and no locks. There is no background work yet.

## Planned (not implemented)
- Background indexing on snapshots with bounded channels and cancellation; results published
  as "index of document D at revision R", dropped or marked stale when R is behind.
- Index generation counter per workspace.
- Change subscriptions (`nvim_buf_attach`-style) carrying `previousRevision` so clients can
  detect gaps.
- Agent proposals as edit sets against a named base revision, applied only through the same
  revision check (they can never silently overwrite newer human edits).
