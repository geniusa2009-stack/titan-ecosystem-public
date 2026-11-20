# ADR 0011 — Project Twin V1: stable identities, provenance, incremental updates, impact

Status: accepted (2026-11-10). Supersedes the graph part of ADR 0007.

## Decision
- **Identities.** Opaque numeric node ids assigned from semantic keys and kept in an identity
  table that survives rebuilds: packages by name; modules by (package, crate target, module
  path); items by (module, container chain, name, kind, ordinal); tests likewise; files by an
  opaque id. Ids survive line shifts and `x.rs` ↔ `x/mod.rs` moves. A file rename reported by
  the watcher and a symbol rename applied through `refactor.rename` (when invoked on the
  definition) keep identities; unreported renames produce new ids, never a guess.
- **Relations.** contains, defines, imports, tests, dependsOn — each with provenance (source,
  analysis type, evidence kind, file, line, content version, generation) and a freshness
  computed at query time against the current file version. References and calls are not
  stored: they come from rust-analyzer per query (`twin.impact`) with their own status.
- **Incremental maintenance.** Per-file facts (mod declarations, `use` paths, definitions,
  tests) are cached by version. A published parse whose module declarations and `use` paths
  are unchanged updates the file's local contribution in place (measured in µs); structural
  changes trigger a rebuild from cached facts that re-parses only changed files. A randomized
  test (12 seeds × 30 edits) checks incremental == full rebuild after every edit.
- **Compact storage.** Names are `Arc<str>` shared by facts, keys and nodes; module keys are
  shared by all items of a module; paths are interned per file. (Before this, the graph was
  the largest owner of `titand` heap — see `benchmarks/results/2026-11-10-phase4-*`.)
- **Impact analysis** (`twin.impact`): known (rust-analyzer definition, references, callers via
  call hierarchy), structural (module, package, importers, dependents, tests by layout),
  heuristic (same-name definitions), unknown (macros, dynamic dispatch, outside the workspace,
  other languages). `coverage.complete` is always false.

## Limits
Rust only; syntax + manifest evidence (no macro expansion, no cfg evaluation, no re-export
following); tests are linked by attributes and layout, not by coverage; identities are kept
for the lifetime of a `titand` process, not persisted.
