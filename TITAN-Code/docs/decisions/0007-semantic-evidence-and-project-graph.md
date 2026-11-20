# ADR 0007 — Semantic evidence model and the project graph

Status: accepted (2026-11-10).

## Context
Phase 3 adds a language server (rust-analyzer) next to the tree-sitter index. The two
disagree by design: syntax knows definitions and nesting at the exact buffer revision;
the language server resolves names but may be loading, restarting, or answering for an
older version.

## Decision
- **No merging.** Every `intel.*` answer is a list of `Evidence` entries, one per source,
  each with `status` (`current`/`stale`/`unavailable`/`failed`/`timeout`), the
  `documentRevision` it describes, the source `generation`, `complete` (rust-analyzer
  quiescent, or index idle) and explicit `limitations`. No confidence percentages.
- **Revision identity.** The LSP `version` of a document is its TITAN revision. Requests
  carry the revision they were computed for; an answer that arrives after the document
  changed, or from an older server generation, is `stale`. Diagnostics are `current` only
  when their `version` equals the current revision.
- **Buffers, not disk.** Open Rust documents are synchronised from their buffers; a new
  server generation gets `didOpen` with the current text.
- **Name matches are labelled.** The syntax fallback for definitions is `nameMatch`:
  definitions with the same identifier, explicitly not resolved.
- **Project graph** (`titan-index::graph`): workspace, packages, crate-root modules, module
  trees, files, symbols, tests; edges `contains`/`defines`/`imports`/`tests`, each with the
  evidence it came from (file and line). Built in the background from disk + open buffers,
  tagged with a generation and a `stale` flag; rebuilt after scans, file events and
  published parses (at most once per second). No graph database: an in-memory adjacency
  list answers the queries in microseconds for workspaces of this size.

## What the graph does not claim
Macro-generated items, `#[path]`, `cfg` evaluation, re-exports, symbol-level references
(use `intel.references`), dependencies outside the workspace.

## Alternatives
- Merging LSP and syntax results into one list: hides which source said what.
- Using the language server for the graph: not available while it loads or after it
  fails, and its symbol view is per request, not a persistent structure.
- A graph database: no demonstrated need at current sizes.

## Evidence
`crates/titan-server/tests/semantic.rs` (real rust-analyzer: definition, references,
hover, symbols, completion, diagnostics per revision, kill + restart; fake server: crash
loops, hangs, cancellation, stale answers, unavailable server),
`crates/titan-index/src/graph.rs` tests, `crates/titan-server/tests/watch.rs`
(`project_graph_is_built_and_queried`).
