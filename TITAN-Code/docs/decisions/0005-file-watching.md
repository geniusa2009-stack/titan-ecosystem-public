# ADR 0005 — Workspace file watching

Status: accepted (2026-11-10).

## Context
`titand` keeps documents in memory. Files also change on disk behind its back: `git checkout`,
formatters, code generators, other editors, and our own atomic saves. The event loop needs to
hear about these changes so it can reload clean buffers, flag conflicts in dirty ones and keep
the file tree and index current. It must not be flooded: a checkout can touch thousands of
files, and a build writes into `target/` continuously.

Requirements:
- Linux and macOS first, Windows later.
- Bounded memory.
- An explicit "you lost events, rescan" signal.
- Nothing outside the workspace.
- Editor temp files (including our own `.titan-save-*`) suppressed.
- No async runtime, in line with ADR 0003.

## Decision
A new crate, `titan-watch`, on top of **`notify` 8.x** (`RecommendedWatcher`: inotify, FSEvents,
`ReadDirectoryChangesW`), with **its own coalescing layer** instead of
`notify-debouncer-full`.

- **Threads.** The backend callback does a non-blocking `try_send` into a bounded queue
  (`RAW_QUEUE_CAPACITY` = 16 384 events). If the queue is full, the event is dropped and a flag
  is set, which ends the current window in a `Rescan`. One aggregator thread coalesces the
  events and calls the consumer's callback. It never holds a lock while doing so, catches
  panics, and stops calling once `stop()` returns.
- **Coalescing windows.** A window is a trailing debounce (default 50 ms), capped at 10 × debounce so a
  continuous stream still produces output. Within a window there is one entry per path. The
  final kind is decided at flush time from two inputs: the first event seen and an `lstat` of
  the path at that moment. The rules:
  - gone → `Removed`;
  - first event was a create or move-in → `Created`;
  - first event was a removal → `Modified` (replaced);
  - otherwise → `Modified`.

  A paired rename whose target still exists becomes `Renamed { from }`. Chains collapse.
  Removed descendants of a removed directory fold into the directory.
- **Bounds.** At most `max_pending` (default 10 000) distinct paths per window. The next
  distinct path drops the detail, and the window ends in exactly one `Rescan`. Kernel overflow
  (`IN_Q_OVERFLOW`, FSEvents `MustScanSubDirs`) and backend errors also end the window in a
  `Rescan`. Errors are reported as `Error` as well.
- **Filtering.**
  - Paths must be lexically inside the canonical root; `..` components count as outside.
  - A built-in temp filter is checked on every component: `.titan-save-*`, `.#*`, `#*#`, `*~`,
    `*.swp`/`*.swx`/`*.swo`, `4913`, `*.tmp`, `*.crswap`, `.DS_Store`.
  - The caller's predicate then sees root-relative paths.
  - Rename sources are filtered too, so an atomic save shows up as a change of the target
    only.
- **Symlinks.** The backend runs with `with_follow_symlinks(false)`. At flush time the parent
  directory of every path is canonicalised, and the path is dropped if that parent resolves
  outside the root.
- **FSEvents rename pairing.** FSEvents has no rename cookie, so rename halves are paired
  heuristically. Two back-to-back `Name(Any)` halves pair when exactly one side exists; the pair
  runs missing → existing. The half survives the cumulative Create/Remove flags that FSEvents
  expands before the rename flag of the next record. An unpaired half degrades to
  `Removed`/`Created` by existence.

## Alternatives considered
- **`notify-debouncer-full` 0.7.** Its source was read before deciding. Its strengths: rename
  stitching via a file-id cache, and one `Remove` per deleted directory. Reasons not to use it:
  - Its per-path queues are unbounded, and it has no `max_pending` or overflow contract. It
    only forwards notify's rescan event.
  - Its `RecommendedCache` walks and stores file ids for the *whole* tree at start and on
    every rescan. That includes ignored directories such as `target/`, so memory and start-up
    cost scale with the build output.
  - It emits individual notify events, which may include several per path, rather than one
    resolved change per path.
  - It uses `unwrap()` on its internal mutex and in the rename path.

  We borrowed its ideas instead: pairing by tracker (cookie), folding a directory's removed
  children into the directory, and treating Create-then-Modify as Create.
- **Polling** (`notify::PollWatcher` or our own walker). Portable and immune to watch limits,
  but latency is proportional to tree size times the polling interval, and CPU is spent on idle
  trees. It stays available as a future fallback: when inotify limits are hit, or on network
  filesystems where kernel notifications do not work.
- **Watchman.** Excellent at scale (crawls, cookies, settle periods), but it is an external
  daemon with its own install and versioning, plus a JSON or BSER socket protocol. That
  conflicts with "core editing works offline with nothing else installed". It may become an
  optional backend for huge monorepos.
- **Own inotify/FSEvents bindings.** Would give full control: per-directory watches that skip
  ignored directories, and FSEvents event ids. The cost is `unsafe` FFI on two platforms. Our
  lints forbid `unsafe` without an audited reason, and notify already handles these correctly.
  Revisit if the ignored-directory watch cost becomes a measured problem.

## Limits (known, documented in the crate)
- Changes are hints. A consumer must `stat` or read the file before acting. A file created and
  deleted within one window is reported as `Removed`. Created and Modified are not reliably
  distinguished on macOS, because FSEvents flags are cumulative.
- **inotify watches every directory, including ignored ones**, so `fs.inotify.max_user_watches`
  can be exhausted by a large `target/`. At start-up this fails with `WatchError::WatchLimit`;
  later it produces `Error` + `Rescan`, and new directories go unwatched.
- **New directories (inotify):** files created inside a new directory before its watch is
  added are not reported. Consumers should scan directories reported as `Created` or
  `Renamed`.
- **FSEvents pairing is heuristic.** A rename across the root boundary, or one interleaved with
  other events, degrades to `Removed` + `Created`.
- notify 8.2's FSEvents callback `expect`s UTF-8 paths. A non-UTF-8 name under the root
  would therefore panic inside an `extern "C"` callback, which aborts the process. This is
  upstream behaviour and our tests do not exercise it.
- notify's Windows backend does not pair rename halves, so a rename is reported as
  `Removed` + `Created`. Windows is compiled through notify but not tested in CI.
- `stop()` joins our thread and FSEvents' run-loop thread. notify's inotify thread is detached;
  it exits shortly after its shutdown message. The Linux test waits for the thread count to
  return to baseline.

## Evidence
- Unit tests in `crates/titan-watch/src/coalesce.rs` feed synthetic inotify- and
  FSEvents-shaped sequences and check the coalescing rules, rename pairing (inotify cookie,
  FSEvents halves including cumulative flags), atomic save, overflow → one `Rescan`, backend
  rescan and errors, outside-root and ignored paths, symlink escape and directory folding.
- `crates/titan-watch/tests/watch.rs` runs on the real filesystem, on Linux (inotify, `test`
  job) and macOS (FSEvents, `watch-macos` job). It covers: create/modify/delete, rename,
  atomic replacement, directory create/rename/recursive delete, a 500-write burst, overflow
  with `max_pending = 10`, ignore and temp filters, events outside the root, symlinks out of
  the root, and prompt `stop`.
- `tests/stop_threads.rs` (Linux) checks that the thread count returns to baseline after
  `stop`.

### Observed on CI
CI run 42708811224 used GitHub-hosted `ubuntu-latest` and `macos-latest` runners, Rust
1.91.0, and resolved `notify` 8.2.1 / `notify-types` 2.1.0. Resolver 3 is MSRV-aware: notify 8.2.2
declares `rust-version` 1.88, above the workspace's 1.85, while 8.2.1 declares 1.85.
All titan-watch tests passed on both platforms. The Linux column shows what the tests
*assert* there; the Linux job does not print observations. The macOS column is what the tests
printed with `--nocapture`.

| Case | Linux (inotify) | macOS (FSEvents) |
|---|---|---|
| Create a file | `Created` | `Created` |
| Append to a reported file | `Modified` | `Created` (cumulative flags, as expected) |
| Delete | `Removed` | `Removed` |
| File rename in the root | `Renamed { from }`, asserted strictly | `Renamed { from }`, paired by the heuristic |
| Directory rename | `Renamed { from }`, `is_dir: Some(true)` | `Renamed { from }`, `is_dir: Some(true)` |
| Atomic save | `Created` or `Modified` for the target (asserted), temp never visible | `Created` for the target, temp never visible |
| 500 appends | 1–5 changes (asserted), coalescing visible in stats | 1 change in 1 batch (10 raw events, 9 coalesced) |
| 200 files with `max_pending = 10` | exactly 1 `Rescan` (asserted), at most 10 changes | exactly 1 `Rescan`, 0 changes |
| `stop()` | under 2 s (asserted); thread count back to baseline | returned in about 1 ms |

## Replacement criteria
Replace or extend this design if any of the following is observed:
- inotify watch exhaustion in real workspaces. Then do per-directory watching that skips
  ignored directories, or fall back to polling.
- Measured FSEvents mis-pairing that matters to users. Then use event ids or a file-id cache
  restricted to open documents.
- A need for multi-root workspaces or network filesystems.
