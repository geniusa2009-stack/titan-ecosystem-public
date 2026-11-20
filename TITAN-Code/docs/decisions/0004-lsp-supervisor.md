# ADR 0004 — Supervised language-server client (`titan-lsp`)

Status: accepted (2026-11-10).

## Context
Phase 3 adds semantic features (definition, references, hover, symbols, completion,
diagnostics) by talking to external language servers, starting with rust-analyzer. A
language server is a large third-party process: it can be slow to load, answer out of order,
send `ContentModified`, hang, write garbage, or crash. TITAN Code's rule is that core editing
works without any language server, so the server must be treated as untrusted and
crash-prone, and nothing it does may block `titand`'s event loop.

## Decision
A new crate `crates/titan-lsp` (depends only on `titan-protocol`, `serde_json`, `thiserror`)
provides `Supervisor`, a client for one LSP 3.17 server process.

**Threads, no async runtime.** One control thread per supervisor owns lifecycle, timers and
the restart policy, and is the only thread that emits events (one total order; the callback
never runs under an internal lock; callback panics are caught). Each process generation has a
reader thread (`titan_protocol::framing::read_message` with a body cap, then JSON), a writer
thread fed by a bounded queue (one framed message per write) and a stderr thread that drains
into a 64 KiB ring buffer for crash reports. Callers only take a short lock and `try_send`;
`request` returns a ticket immediately, results arrive as events.

**Exactly-once completion.** Every ticket completes once: answer, `Timeout` (deadline enforced
by the control thread, which also sends `$/cancelRequest`), `Cancelled` (explicit, also sends
`$/cancelRequest`), `ConnectionLost` (process died) or `Stopped` (shutdown). Responses are
matched by id; late answers and unknown ids are counted and dropped.

**Server-to-client requests are always answered** (`workspace/configuration` from config by
section, registration and progress creation with `null`, unknown methods with `-32601`), so a
server never stalls waiting for the client.

**Lifecycle.** spawn (program + args, never a shell) -> `initialize` (with a deadline;
`clientInfo.name = "titand"`, `positionEncodings: ["utf-16"]`,
`experimental.serverStatusNotification`) -> `initialized` -> Running. rust-analyzer's
`experimental/serverStatus` is surfaced as `Running { quiescent: Option<bool> }` — `None`
when the server never reports, so callers are not told a server is ready when it did not say
so.

**Crash policy.** EOF, exit, framing error, oversize message, malformed JSON, non-JSON-RPC
message, `initialize` error or timeout -> pending requests fail with `ConnectionLost`,
`Crashed { stderr_tail }` is emitted, and the server restarts after exponential backoff.
More than `max_restarts` crashes inside `restart_window` -> `Failed` (no more attempts). A
program that cannot be executed (not found / permission denied) fails immediately. Each
restart increments the **generation**; the caller must re-send `didOpen` when it sees
`Running` for a new generation (the supervisor does not hold document state — `titand` does).
A 250 ms liveness poll catches a process that died while a child keeps its `stdout` open.

**Shutdown.** `shutdown` -> `exit` -> close stdin -> wait up to `shutdown_timeout` -> kill;
the child is reaped before `shutdown()` returns. Dropping the supervisor kills immediately.

**Testing with a fake server.** `titan-fake-lsp` (a binary of the crate, flags only, no
analysis) injects failures deterministically: crash after initialize or on a method, hang,
delay, reverse order, garbage / bad header / oversize output, slow or ignored shutdown,
server-to-client requests, server status. Real behaviour is verified against rust-analyzer.

## Alternatives considered
- **tokio + tower-lsp / async-lsp:** tower-lsp is for writing servers, not clients. An async
  client would need a runtime in `titand`, which ADR 0003 deliberately avoided; the workload
  (a handful of pipes per server) does not need one, and explicit threads keep deadlines and
  kill semantics easy to test.
- **`lsp-types`:** complete and typed, but large, with frequent breaking releases, and it
  forces conversions at every boundary. The supervisor only needs envelopes; feature code can
  add small typed structs (or `lsp-types`) later without changing the transport.
- **`lsp-server` (rust-analyzer's transport crate):** synchronous and small, but it is written
  for the server side, uses crossbeam channels and has no supervision, timeouts or restart
  policy, which are the point of this crate.
- **Restart forever with backoff:** rejected; an unavailable server must reach a terminal
  `Failed` state the caller can show instead of burning CPU.

## Limits
- One server process per `Supervisor`; routing documents to servers is the caller's job.
- Only `utf-16` is offered as position encoding; callers must convert with the
  `Utf16Point` types from `titan-text`.
- The outgoing queue holds 4096 messages by default; when full, calls fail with `QueueFull`.
- A server that stops reading its input but keeps running is noticed only through request
  timeouts, not as a crash.
- After a crash the supervisor does not replay `didOpen`/`didChange`; the caller must.
- `on_event` runs on the control thread: a slow callback delays all events and timers.

## Evidence
`crates/titan-lsp/tests/fake_server.rs` (handshake and client capabilities, out-of-order
matching, unknown ids, timeout + `$/cancelRequest`, explicit cancel with a late answer,
crash -> `ConnectionLost` -> restart -> new generation -> resync, crash loop -> `Failed`,
missing executable, garbage / malformed header / oversize output, graceful and forced
shutdown, drop kills, contained callback panics, no leaked processes via `kill -0`), and
`crates/titan-lsp/tests/rust_analyzer.rs` against the rust-analyzer of the pinned toolchain
in CI (navigation in an unsaved buffer, references, hover, symbols, completion, versioned
diagnostics, `kill -9` -> restart -> resync, clean shutdown).

## Replacement criteria
Many concurrent servers per process (thread count becomes a cost) or a move of `titand` to
an async runtime for other reasons; a need for full typed LSP models across features.
