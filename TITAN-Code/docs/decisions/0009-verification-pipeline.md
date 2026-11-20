# ADR 0009 — Controlled verification pipeline (`titan-verify`)

Status: accepted (2026-11-10).

## Context
Phase 4 needs evidence that a proposed change (typically produced by an agent) is better or
worse than the code it replaces: does it compile, do the tests pass, which tests changed.
Running checks means running a compiler and, for tests, the candidate's own code. The
candidate and the repository content are therefore *untrusted input*: nothing in them may
choose which programs run, and code that runs implicitly during compilation (build scripts,
proc macros, cargo configuration) must be an explicit decision of the caller.

## Decision
A new crate `crates/titan-verify` (depends on `serde`, `serde_json`, `thiserror`, `tempfile`,
`toml`, `blake3`, and `rustix` with `process` on Unix) provides `Workspace::run(plan)`:

1. validate the plan (checks, paths, revision, timeouts) before anything is created;
2. resolve the base revision to a commit (`git rev-parse --verify --quiet --end-of-options
   <rev>^{commit}`);
3. create a private run directory (`tempfile`) with two detached worktrees of that commit,
   `base` and `candidate` (`git worktree add --detach <path> <sha>`);
4. apply the candidate (`FileOp::{Write, Delete, Rename}`) to `candidate`, recording BLAKE3
   of every written file and a patch id (BLAKE3 over a canonical encoding of the ops);
5. inspect each worktree (trust inspection, below);
6. run every planned check on the baseline, then on the candidate, one process at a time;
7. compare per check, remove both worktrees (`git worktree remove --force`, delete the run
   directory, `git worktree prune`) and return a serde-serializable `VerificationReport`.

Worktrees are removed by `PreparedWorktrees::cleanup` and by its `Drop`, so every early
return (git failure, unsafe patch path) also cleans up. If the process itself is killed,
`git worktree prune` (run at the end of every later run) removes the stale metadata; the
run directory stays in the temp directory.

### Safety model
**No arbitrary command execution.** A check is a variant of a fixed, typed catalogue:
`CargoCheck { package }` (`cargo check --all-targets`), `CargoTest { package, test_filter }`
(`cargo test --no-fail-fast [-- <filter>]`), `CargoBuild`, `CargoFmtCheck`
(`cargo fmt --all -- --check`), `CargoClippy` (`-- -D warnings`). Each maps to one fixed
argument vector. The only free arguments, package name and test filter, must match
`[A-Za-z0-9_:-]+`, 1..=128 bytes, and must not start with `-` (so they cannot become an
option). Revisions must match `[A-Za-z0-9_./~^@{}-]+` without a leading `-`. Programs are
`git`, `cargo` and — only for `rustc --version` in the report — `rustc`, each from an explicit
absolute path in `VerifyConfig` or the first match on an *absolute* `PATH` entry (relative
entries are ignored, so the worktree can never supply a binary). `std::process::Command`
is used directly; no shell is ever involved.

**Git invocations** have fixed argument shapes, run with `-c core.hooksPath=/dev/null
-c core.fsmonitor=false` (so `post-checkout` hooks and fsmonitor commands do not run) and an
environment of `PATH`, `HOME`, `GIT_TERMINAL_PROMPT=0`. The user's own git configuration
(`~/.gitconfig`, the repository's `.git/config`, filter drivers it defines) is trusted.

**Candidate paths** are validated lexically (relative, `/`-separated, no empty, `.` or `..`
component, no `\`, NUL or drive prefix, no `.git` component in any letter case, ≤ 4096
bytes) and again while applying: no existing parent component may be a symbolic link and a
written file may not replace a symbolic link (renames refuse non-regular sources). The
worktree is private to the run, so nothing changes it between the check and the write.

**Trust policy.** `TrustPolicy { allow_build_scripts, allow_proc_macros, allow_network }`,
all `false` by default. Before any check that compiles code (`check`, `test`, `build`,
`clippy`) each worktree is inspected:
- every `Cargo.toml` (walk without following symlinks; `toml` parse): `package.build`
  (`true`, a path, a list) or a `build.rs` next to the manifest when `build` is absent;
  `[lib] proc-macro = true`;
- the resolved dependency set with `cargo metadata --format-version 1 --offline`, run under
  the same controls as a check: any package with a `custom-build` target (build script) or
  a `proc-macro` target, and path packages whose manifest resolves outside the worktree.
  When metadata succeeds, only packages in the resolved set count (a stray manifest in a
  fixture directory does not block); when it fails, the check is not run;
- cargo configuration files (`.cargo/config`, `.cargo/config.toml`) in the worktree root
  and every ancestor directory, except `$CARGO_HOME`'s own; they can set
  `build.rustc-wrapper`, linkers, runners, or an alias that shadows `cargo fmt`;
- `rust-toolchain` / `rust-toolchain.toml` in the worktree and its ancestors, which select
  the toolchain binaries rustup runs.

A compiling check is recorded as `NotRun { reason }` when the worktree needs build scripts
or proc macros the policy does not allow, when the inspection is incomplete (unparsable
manifest, metadata failure), or when a path package lies outside the worktree. Cargo
configuration files, and toolchain files without a pinned `RUSTUP_TOOLCHAIN`, make *every*
check `NotRun`, `cargo fmt --check` included (`cargo fmt` is an external subcommand an alias
can replace). `cargo fmt --check` otherwise runs regardless of the policy: it compiles and
executes no project code. `NotRun` is never counted as passed; its comparison verdict is
`Incomparable`.

**Environment.** Every child starts from an empty environment (`env_clear`). Checks get
only `PATH`, `HOME`, `RUSTUP_HOME`, `CARGO_HOME`, `RUSTUP_TOOLCHAIN` (each only if set in the
parent; `RUSTUP_TOOLCHAIN` may be pinned in `VerifyConfig`), `CARGO_TARGET_DIR` (one per side,
inside the run directory), `CARGO_TERM_COLOR=never` and, without `allow_network`,
`CARGO_NET_OFFLINE=true` plus `--offline`. The working directory is the worktree; stdin is
`/dev/null`.

**Resource limits.** Each check has a wall-clock timeout (including compilation). On Unix the
child is spawned with `CommandExt::process_group(0)`; on timeout the whole group is killed
with `SIGKILL` (`rustix::process::kill_process_group`) *before* the leader is reaped, so the
group id cannot have been reused. After a normal exit the group is killed once more, best
effort, to remove stragglers (e.g. a background process started by a test). Output of stdout
and stderr is read continuously by two threads and kept as the first and last N KiB
(default 64 + 64) with `total_bytes`, `omitted_bytes`, `truncated`. libtest counts are
parsed from the *whole* stdout stream, not from the bounded log.

### This is not a sandbox
The pipeline controls *what* may run (catalogue + trust policy), *where* (a private
worktree and target directory), *with which environment*, and *for how long*. It provides no
system-call, file-system, network or privilege isolation:
- `cargo test` executes the candidate's test code; an allowed build script or proc macro
  executes at build time. That code runs with the caller's user, can read and write any file
  the user can (including outside the worktree), and can open network connections
  (`--offline` only stops cargo itself from downloading);
- a process that leaves the process group (`setsid`, `setpgid`, double fork with a new
  group) or is started through another service survives the timeout kill;
- memory, CPU, disk and file-descriptor use are not limited;
- on non-Unix platforms only the direct child is killed (not tested; CI covers Linux and
  macOS).
Running untrusted candidates safely additionally needs an OS sandbox (container, VM,
seccomp/landlock, macOS sandbox profiles) around the whole run; the report format does not
change when one is added.

### Comparison
Per planned check: `Passed/Passed` → `Same`; baseline `Failed|TimedOut` and candidate
`Passed` → `Improved`; baseline `Passed` and candidate `Failed|TimedOut` → `Regressed`; both
`Failed` → by failing test names (any new failing test → `Regressed`, otherwise fewer →
`Improved`, otherwise `Same`; by counts when the name list was truncated; `Same` when neither
side produced test results, e.g. both fail to compile); anything with `NotRun`, `Error`, both
`TimedOut` or `Failed` vs `TimedOut` → `Incomparable`. `newly_failing` / `newly_passing` list
test names.

### Baseline = a commit
The baseline is always a commit checked out in its own worktree; uncommitted changes of the
user's working tree are not part of it. Callers that want to verify uncommitted work express
it as (part of) the candidate.

## Alternatives considered
- **Run checks in the user's working tree:** no isolation between baseline and candidate,
  and a failed run could leave the user's files modified. Rejected.
- **`git stash` / `git checkout` in one clone:** mutates shared state and the stash stack;
  worktrees give two independent checkouts of one object store without copying.
- **Accept command strings from a configuration file (`verify.toml`):** that is arbitrary
  command execution chosen by repository content. Rejected; new checks are new enum variants
  reviewed in code.
- **Parse `Cargo.lock` instead of `cargo metadata`:** the lock file lists packages but not
  whether they have build scripts or proc-macro targets; that needs their manifests, which
  `cargo metadata --offline` reads from the local registry cache. The manifest walk is kept
  as an independent check for workspace packages.
- **libtest JSON output (`-Z unstable-options --format json`):** nightly-only. The human
  format's summary and `... FAILED` lines are stable enough and covered by unit tests.
- **OS sandboxing now (bubblewrap, landlock, sandbox-exec):** platform-specific and not
  available on every developer machine; recorded above as the next layer, not claimed.

## Limits
- Only cargo checks; one workspace rooted at the repository root.
- File contents in `FileOp::Write` are UTF-8 text; file modes are not set (new files get the
  default mode; renamed files keep theirs).
- A test named in a panic message line that looks exactly like `test result: ok. ...` would
  be counted; libtest captures test output by default, so this needs `--nocapture`, which the
  catalogue does not offer.
- Output interleaving between stdout and stderr in the log is approximate.
- Version queries (`git`, `cargo`, `rustc --version`) run in the run directory, outside the
  worktrees.
- If the pipeline's own process is killed, the run directory is left behind (worktree
  metadata is pruned by the next run).

## Evidence
`crates/titan-verify/tests/pipeline.rs` (Linux and macOS CI) runs real `git` and nested
`cargo` on throw-away repositories: a candidate fixing a failing test (`Failed` → `Passed`,
`Improved`, counts summed over three test binaries, log truncated while counts stay exact), a
candidate breaking compilation, a timeout that kills the test binary and its child (`kill -0`
on both pids), build-script and proc-macro gating (a build script writes a marker file that
must not exist under the default policy), a candidate `.cargo/config.toml`, environment
scrubbing (a secret in the parent environment is invisible to the tests), worktree removal
after success, failure and drop, and plan validation. Unit tests cover argument and path
validation, symbolic-link escapes, log truncation, the libtest parser, manifest inspection and
comparison verdicts.
