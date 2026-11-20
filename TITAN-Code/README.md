# TITAN Code

Headless editor core and engineering backend, written in Rust.

TITAN Code is the foundation of a future software engineering environment: a revision-aware
text engine, document persistence, workspace services, a syntax index, and a versioned
JSON-RPC protocol that a native frontend can consume later. **There is no frontend in this
repository by design.**

- Rules for contributors and coding agents: [`CLAUDE.md`](CLAUDE.md)
- What actually works today: [`docs/STATUS.md`](docs/STATUS.md)
- Protocol reference: [`docs/protocol.md`](docs/protocol.md)
- Architecture decisions: [`docs/decisions/`](docs/decisions/)
- Competitor source research: [`research/`](research/)

## Quick start

```bash
cargo test --workspace
cargo run -p titan-server --bin titan-probe -- --report target/probe-report.json
```

`titan-probe` creates a temporary project, drives the real `titand` service over stdio
JSON-RPC (open → edit → undo → redo → save → reopen → search → index), and writes an
auditable JSON report.

```bash
cargo run --release -p titan-bench -- --out target/bench-report.json
```

## Crates

| Crate | Role |
|---|---|
| `titan-text` | Rope buffer, coordinate units, transactions, revisions, undo/redo, snapshots |
| `titan-doc` | File load/save: encoding checks, line endings, external changes, atomic writes |
| `titan-workspace` | Safe paths, ignore-aware file listing, search |
| `titan-index` | tree-sitter Rust symbol index (syntax-level evidence only) |
| `titan-protocol` | Framing, JSON-RPC envelope, error codes, typed messages |
| `titan-server` | `titand` service, client, `titan-probe` |
| `titan-bench` | Reproducible benchmark harness |

## License

Proprietary, all rights reserved, until a distribution license is chosen. Third-party
components keep their own licenses; see `docs/source-analysis/REUSE_REGISTER.md`.
