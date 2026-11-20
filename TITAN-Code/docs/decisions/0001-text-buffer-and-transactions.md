# ADR 0001 — Text buffer and transaction model

Status: accepted (2026-11-09). Evidence: `research/TEXT_ENGINE_RECOMMENDATION.md`,
`research/zed/*`, `research/helix/*`, `research/vscode/piece-tree.md` on branch
`research/editor-architecture`. Those research notes are preliminary: their line citations
were produced by research agents and have not all been re-checked by hand.

## Context
The core needs cheap immutable snapshots for background work, multi-range transactions
with undo, validated UTF-8 / UTF-16 / line conversions, and revision-checked edits. It is
single-writer for now; collaboration comes later.

## Alternatives examined
1. **ropey rope + Helix-style edit list with stored inverses.** Mature crate, O(log n) edits
   and conversions, O(1) `clone` with structural sharing.
2. **Zed-style SumTree rope with CRDT fragments.** Built for collaboration; every edit pays for
   tombstones, Lamport clocks and version vectors; the `text`/`rope` crates are GPL-3.0.
3. **Own piece tree (VS Code design).** VS Code's implementation uses UTF-16 offsets, copies
   the line-start array on every insert (`pieceTreeBase.ts` per the research notes), needs
   about ten CRLF repair sites, and its snapshots copy the piece list.

## Decision
Option 1, written byte-first:
- `TextBuffer` wraps `ropey::Rope` (Unicode line breaks disabled; LF, CRLF and CR count as
  line breaks, matching LSP). Replaceable: everything outside `buffer.rs` uses the
  `TextBuffer` API, not ropey.
- Canonical unit is the UTF-8 byte (`ByteOffset`). `CharOffset`, `Utf16Offset`, `Point`
  (byte column) and `Utf16Point` exist only at the edges and are converted through functions
  that return errors for non-boundary or out-of-range input; nothing is clamped silently.
- A transaction is a `Vec<TextEdit>` (byte range + replacement) against one base revision.
  Edits are validated, sorted (stable), checked for overlap, then applied from the highest
  offset down. Inverse edits are computed before application.
- `TextDocument` holds a monotonic `revision`, a `content_id` that undo/redo restore (VS Code's
  `alternativeVersionId` idea, for O(1) dirty checks) and linear undo/redo stacks of
  `(forward, inverse)`. Every mutation names the revision it was computed against.

## Limitations
- Linear history (no undo tree), no grouping, no memory budget for history yet.
- No `compose`/`transform` of transactions, so concurrent edits are rejected, not rebased.
- No anchors/marks that move with edits yet.
- Grapheme queries cost O(line length), which is slow on multi-megabyte single lines.
- `TextBuffer::text()` copies the whole document; it is used for reads and search overlays,
  not on the edit path.

## How it is verified and measured
- Unit tests for each conversion and edit rule, including Arabic, emoji, ZWJ sequences,
  combining marks, CRLF/CR and very long lines.
- Differential tests against a `String` reference with fixed seeds
  (`crates/titan-text/tests/differential.rs`).
- Benchmarks: not yet in place (see `docs/STATUS.md`). Planned: open, random insert/delete,
  sequential typing, undo/redo, snapshot, offset/line conversion on 1 MB, 10 MB, single-line
  and Unicode-heavy inputs, recording median and p95.

## What would justify replacing it
Benchmarks showing ropey's UTF-16 or line conversions, or snapshot memory, are a bottleneck
at the target sizes, or a collaboration requirement that needs CRDT semantics (peer-to-peer
or offline merge) rather than server-side transformation.
