# ADR 0002 — Headless protocol and service model

Status: accepted (2026-11-09). Evidence: `research/PROTOCOL_AND_STATE_RECOMMENDATION.md`,
`research/neovim/extmarks-and-rpc.md`, `research/lapce/proxy-and-rpc.md` (preliminary).

## Context
A future native frontend, test harnesses and agents need a stable, versioned way to drive the
core without linking Rust code.

## Alternatives examined
- **MessagePack-RPC (Neovim).** Compact, but no length prefix, and the notes show a decode error
  closes the channel. Harder to debug by hand.
- **Newline-delimited JSON (Lapce core↔proxy).** Simple, but per the notes one bad line kills the
  reader thread, and edits are notifications that are silently dropped on revision mismatch.
- **JSON-RPC 2.0 + Content-Length (LSP base protocol).** Widely understood, debuggable,
  length-prefixed so bodies can be bounded and skipped.

## Decision
JSON-RPC 2.0 with `Content-Length` framing over stdio, behind a transport-independent framing
module so Unix sockets can be added later. Protocol version negotiated in `session.hello`, plus
the position encoding (UTF-8 or UTF-16). Every mutation is a request (never a fire-and-forget
notification) carrying `expectedRevision`; mismatches are explicit errors with both revisions.
Errors use stable numeric codes (`docs/protocol.md`). No arbitrary command execution, no
network listener.

The service is single-threaded per connection: one request is completed before the next is
read, so each document has one writer and every response describes one consistent revision.

## Limitations
No change notifications or subscriptions yet; no cancellation (requests are short and
synchronous); one client per process.

## Replacement criteria
Measured protocol overhead dominating latency for large documents (then add incremental
reads or a binary encoding), or a need for multiple concurrent clients.
