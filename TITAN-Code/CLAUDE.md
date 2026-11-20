# TITAN Code — rules for Claude Code sessions

TITAN Code is an independent, headless editor core and engineering backend written in Rust.
Guiding idea: *from editing software to engineering software with evidence.*

## Hard boundaries (never violate)
- **No frontend.** No Qt/QML, GPUI windows, Electron/Tauri, React/HTML/CSS, Monaco, themes,
  panels, renderers, mockups, or empty `frontend/` folders. The only binaries allowed are the
  headless service (`titand`) and diagnostic CLIs that exercise backend code paths.
- **Independent repo.** Never read from, modify, or depend on `geniusa2009-stack/TITAN`
  (TITAN Engine), Unreal Engine, or any game-engine code.
- **No fabrication.** No invented benchmark numbers, CI results, research findings, or
  "implemented" claims. `docs/STATUS.md` must match what tests actually prove.
- Core editing must work offline with no AI provider, network, or language server.

## Layout and dependency direction
```
titan-text      text buffer, coordinates, edits, transactions, history   (no I/O)
   ↑
titan-doc       document identity, revisions, line endings, encoding, atomic save
   ↑
titan-workspace workspace root, safe paths, file enumeration (ignore rules), search
titan-index     tree-sitter syntax index (Rust), symbol graph with provenance
   ↑
titan-protocol  JSON-RPC 2.0 message types + Content-Length framing        (no I/O policy)
   ↑
titan-server    session state + method dispatch; binary `titand` (stdio)
```
Lower crates never depend on higher ones. `titan-text` must stay free of filesystem and
async runtime dependencies. Create a new crate only when it holds working code.

## Conventions
- Rust edition 2024, stable toolchain pinned in `rust-toolchain.toml`.
- Explicit coordinate newtypes (`ByteOffset`, `CharOffset`, `Utf16Offset`, `Point`,
  `Utf16Point`). Never mix units; convert through validated functions that return `Result`.
- No `unwrap()`/`expect()`/panics on production paths reachable from protocol input.
- Errors via `thiserror` enums; protocol maps them to stable numeric codes (see
  `docs/protocol.md`).
- Every mutation names the revision it was computed against; stale revisions are rejected.
- No `unsafe` without a `// SAFETY:` invariant and a test.
- Reused upstream code (Zed, VS Code, Helix, Lapce, Neovim) lives in clearly marked modules
  with an origin header: upstream repo, commit SHA, path, SPDX license, modifications. Record
  it in `docs/source-analysis/REUSE_REGISTER.md`.

## Commands
```
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace
cargo run -p titan-server --bin titan-probe -- --report out.json   # headless e2e demo
cargo bench -p titan-text                                         # benchmarks (release)
```

## Workflow
- `main` is the integration branch; work on `feat/*` or `research/*` branches and open PRs.
  Never merge unreviewed PRs automatically.
- Increment loop: inspect → implement smallest complete change → targeted tests → full
  `cargo test` + clippy → commit → update `docs/STATUS.md`.
- Consequential design choices get an ADR in `docs/decisions/` (alternatives, choice, why,
  limits, how it is benchmarked, what evidence would replace it).
- Performance claims require a benchmark in `benchmarks/` with seed, dataset shape, build
  profile, machine info, and raw results. Never compare to another editor without running it.

## Definition of Done
Builds; fmt + clippy clean; tests pass (none silently skipped); behavior reachable through the
protocol has an integration test; docs and `docs/STATUS.md` updated; existing working features
still pass.
