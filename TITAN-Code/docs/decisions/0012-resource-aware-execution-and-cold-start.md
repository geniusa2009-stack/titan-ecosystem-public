# ADR 0012 — Resource-aware execution and language-server cold start

Status: accepted (2026-11-10).

## Measurements that drove the decisions (CI, 150 × 60 KB + 1 MB Rust workspace)
- Live Rust heap of `titand` (counting-allocator build) per phase: index 32 MiB; **graph
  +153 MiB** (largest owner); 1 MB document + 2 500 edits +12 MiB; 62 MiB still live after
  `workspace.close` because a graph build finishing after the close was published (bug).
- rust-analyzer (separate process): 177–273 MiB in that workspace.
- Cold start without cache priming: ready (quiescent) after 1.2 s, **first definition 6.2 s**
  (lazy analysis), warm definition median 6.7 ms. With cache priming: ready after 8.8 s, first
  definition 25 ms (16 ms in rust-analyzer), warm median 3.8 ms. Time to first answer is
  similar (7.4 s vs 8.9 s); priming moves the cost into the background.

## Decisions
- Compact graph storage (ADR 0011) and **graph epochs**: results of builds started before a
  workspace close/reopen are dropped.
- **Memory-pressure policy** (`/proc/meminfo`, cgroup v2): `high` pauses graph rebuilds and
  drops cached facts of files that are not open; `critical` also releases the graph. Editing,
  journals, open-document parsing and the language server are never shed automatically; the
  language server is controlled explicitly (`intel.server stop|start|restart`).
  `--no-graph`, `--graph-max-files N` (default 20 000), `--workers N`.
- **rust-analyzer cache priming on by default**: `intel.status` reports `indexing` until it
  is done, and the first semantic answer afterwards is warm. The cold-start timeline
  (initialized, ready, first diagnostics, first answer per method) is reported per server
  generation in `intel.status` → `semantic.coldStart`.

## Not done
- Allocator choice (glibc keeps freed memory resident; RSS stays above live heap after a
  close). Changing the allocator is a C dependency and needs its own measurement.
- rust-analyzer's own memory profiling (`RA_PROFILE`) is not enabled in prebuilt binaries;
  only its RSS is measured.
