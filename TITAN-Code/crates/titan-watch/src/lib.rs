//! TITAN Code workspace file watching.
//!
//! [`Watcher::start`] watches one workspace root recursively with the platform backend
//! chosen by the [`notify`] crate (inotify on Linux, FSEvents on macOS,
//! `ReadDirectoryChangesW` on Windows) and delivers **coalesced, de-duplicated batches** of
//! [`FsChange`]s to a callback. The consumer (the `titand` event loop) reconciles them with
//! open buffers; every change is a *hint* to look at the disk, never a substitute for it.
//!
//! # Threads and delivery
//! The backend thread only pushes raw events into a bounded queue (non-blocking). One
//! aggregator thread (`titan-watch`) coalesces them and calls `on_event`; the callback is
//! never called while an internal lock is held, never concurrently, and never after
//! [`Watcher::stop`] returns. A panicking callback is caught and counted.
//!
//! # Coalescing
//! A window opens with the first relevant event and closes when no event arrived for
//! [`WatchConfig::debounce`] or, under a continuous stream, after `10 × debounce` at the
//! latest. Within a window each path yields at most one change. The kind is resolved at the
//! end of the window from what happened *and* from a `stat` of the path at that moment:
//!
//! * the path is gone → [`ChangeKind::Removed`] (also for a file created and deleted within
//!   the window: a consumer that never knew the path ignores it);
//! * it exists and its first event was a create/move-in → [`ChangeKind::Created`]
//!   (Created then Modified ⇒ Created). `Created` may replace an earlier file at the same
//!   path: an atomic save (temp file renamed over the target) is reported as `Created` for
//!   the target, because neither inotify nor FSEvents distinguishes "moved over an existing
//!   file";
//! * it exists and its first event was a removal → [`ChangeKind::Modified`] (replaced);
//! * otherwise → `Modified` (content or metadata changed);
//! * the target of a paired rename that still exists → [`ChangeKind::Renamed`]; the source
//!   is not reported separately. Chains collapse (`a→b→c` ⇒ `c` renamed from `a`, plus
//!   `b` removed), `a→b→a` ⇒ `a` modified.
//!
//! Removed descendants of a removed directory in the same batch are folded into the
//! directory. Directory changes report the directory itself; the consumer decides whether
//! to rescan below it (see the inotify note below). Batches are sorted by path.
//!
//! # Bounds and lost events
//! At most [`WatchConfig::max_pending`] distinct paths are buffered per window. One more
//! drops the buffered detail and the window ends in a single [`WatchEvent::Rescan`]; events
//! for the rest of that window are discarded. The raw queue between backend and aggregator
//! holds [`RAW_QUEUE_CAPACITY`] events; when it is full events are dropped and the window
//! ends in a `Rescan`. Kernel overflows (inotify `IN_Q_OVERFLOW`, FSEvents
//! `MustScanSubDirs`/`UserDropped`/`KernelDropped`) and backend errors also end the window in
//! a `Rescan`; errors are additionally reported as [`WatchEvent::Error`].
//!
//! # Filtering and workspace boundaries
//! Only paths lexically inside the canonical root are reported; paths with `..` components
//! or outside the root are dropped and counted (`outside_root`). Built-in temp/noise files
//! ([`is_temp_name`], e.g. `.titan-save-*`, `*.swp`, `4913`, `.DS_Store`) are dropped when any
//! component of the root-relative path matches, then [`WatchConfig::ignore`] is asked with the
//! **root-relative** path (for example `target/debug/foo`). Both apply to rename sources too:
//! a file moved in from an ignored path is reported as `Created`.
//!
//! **Symlinks** are not followed: the backend is configured with
//! `Config::with_follow_symlinks(false)` (honoured by inotify; FSEvents never follows them),
//! so no watches are placed behind a symlink and writes to its target outside the root
//! produce no events. A symlink inside the root is reported as an entry of its own
//! (`is_dir: Some(false)` — it is the link that changed). As a second line of defence, at
//! flush time the parent directory of every path is canonicalised; a path whose canonical
//! parent lies outside the root is dropped (`outside_root`).
//!
//! # Platform differences
//! * **Ordering** of raw events is not guaranteed across paths; batches are sorted by path
//!   and carry a monotonically increasing `seq`, which is the only ordering promise.
//! * **Rename pairing.** inotify pairs both sides by cookie when both are inside the watched
//!   tree → `Renamed { from }`. notify's Windows backend reports the old and new names as
//!   separate, unpaired events → `Removed(old)` + `Created(new)` (not tested). FSEvents reports
//!   each side separately with no cookie; two back-to-back rename halves where exactly one
//!   side exists are paired heuristically (missing → existing). When pairing fails
//!   (moves across the root boundary, interleaved events) the result is `Removed(old)` +
//!   `Created(new)`.
//! * **macOS FSEvents** delivers with some latency (the stream is created with latency 0
//!   and `NoDefer`, but the kernel still batches) and its per-path flags are cumulative:
//!   a recently created file that is modified may again carry the "created" flag, so a
//!   modification can be reported as `Created`. Consumers must treat `Created` and
//!   `Modified` alike for open buffers.
//! * **Linux inotify** needs one watch per directory. The limit is
//!   `fs.inotify.max_user_watches`; when it is exceeded during start-up
//!   [`Watcher::start`] fails with [`WatchError::WatchLimit`], and when it is exceeded later
//!   (new directories) a [`WatchEvent::Error`] plus a `Rescan` are delivered and the new
//!   directories stay unwatched. Ignored directories (e.g. `target/`) are still watched by
//!   the backend — events are only filtered after the fact. Watches for a new directory are
//!   added after its creation event is read, so files created inside it before that are
//!   not reported: a consumer should scan a directory reported as `Created` or `Renamed`.
//!
//! # Choice of debouncer
//! Coalescing is implemented here rather than with `notify-debouncer-full`: that crate keeps
//! unbounded per-path event queues, walks and caches file ids for the *whole* tree on start
//! (including ignored directories), and emits per-event results without a path bound or a
//! rescan contract. See `docs/decisions/0005-file-watching.md`.

mod coalesce;
mod filter;

use std::fmt;
use std::io;
use std::panic::{self, AssertUnwindSafe};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc::{self, RecvTimeoutError, SyncSender, TrySendError};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use notify::{RecommendedWatcher, RecursiveMode, Watcher as _};

use crate::coalesce::{Coalescer, Flushed};
pub use crate::filter::{TITAN_SAVE_PREFIX, is_temp_name};

/// Capacity of the queue between the backend thread and the aggregator thread, in raw
/// events. When it is full, events are dropped and the current window ends in a `Rescan`.
pub const RAW_QUEUE_CAPACITY: usize = 16_384;

/// Longest the aggregator sleeps while idle before re-checking for shutdown.
const IDLE_POLL: Duration = Duration::from_millis(100);

/// Caller-supplied ignore predicate. It receives paths **relative to the workspace root**
/// (e.g. `target/debug/build`), for every path that would otherwise be reported (including
/// rename sources). Returning `true` drops the path.
pub type IgnoreFn = Arc<dyn Fn(&Path) -> bool + Send + Sync>;

/// Configuration for [`Watcher::start`].
#[derive(Clone)]
pub struct WatchConfig {
    /// Workspace root. It is canonicalised by `start`; reported paths are absolute and lie
    /// under the canonical root ([`Watcher::root`]).
    pub root: PathBuf,
    /// Quiet period that closes a coalescing window. Default 50 ms.
    pub debounce: Duration,
    /// Maximum distinct paths buffered in one window; one more yields a single `Rescan`.
    /// Default 10 000. Must be at least 1.
    pub max_pending: usize,
    /// Caller filter, applied after the built-in temp filter. See [`IgnoreFn`].
    pub ignore: IgnoreFn,
}

impl WatchConfig {
    /// Defaults: 50 ms debounce, 10 000 pending paths, nothing ignored beyond the built-in
    /// temp filter.
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Self {
            root: root.into(),
            debounce: Duration::from_millis(50),
            max_pending: 10_000,
            ignore: Arc::new(|_: &Path| false),
        }
    }

    /// Replaces the ignore predicate.
    pub fn with_ignore(mut self, ignore: impl Fn(&Path) -> bool + Send + Sync + 'static) -> Self {
        self.ignore = Arc::new(ignore);
        self
    }
}

impl fmt::Debug for WatchConfig {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("WatchConfig")
            .field("root", &self.root)
            .field("debounce", &self.debounce)
            .field("max_pending", &self.max_pending)
            .finish_non_exhaustive()
    }
}

/// What happened to a path within one window (see the crate docs for the rules).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ChangeKind {
    /// The path exists now and was created or moved in (possibly replacing an earlier file).
    Created,
    /// The path exists and its content or metadata changed, or it was replaced.
    Modified,
    /// The path no longer exists.
    Removed,
    /// The path exists now and was renamed from `from` (absolute, inside the root), which is
    /// gone. Content may also have changed.
    Renamed {
        /// Previous absolute path.
        from: PathBuf,
    },
}

/// One coalesced change.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FsChange {
    /// Absolute path under the canonical workspace root.
    pub path: PathBuf,
    /// What happened.
    pub kind: ChangeKind,
    /// Whether the path is a directory: from `lstat` when it exists (a symlink is
    /// `Some(false)`), from the backend or from removed descendants when it is gone, `None`
    /// when unknown.
    pub is_dir: Option<bool>,
}

/// Delivered to the `on_event` callback.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum WatchEvent {
    /// One window's changes, one per path, sorted by path.
    Changes {
        /// Monotonic sequence number shared with `Rescan` (starts at 1).
        seq: u64,
        /// The changes.
        changes: Vec<FsChange>,
    },
    /// Events were lost (our bound, the raw queue, the kernel queue, or a backend error).
    /// The consumer must rescan the workspace and re-check open buffers against the disk.
    Rescan {
        /// Monotonic sequence number shared with `Changes`.
        seq: u64,
        /// Why.
        reason: String,
    },
    /// A backend error (always followed by a `Rescan` for the window).
    Error {
        /// Human-readable description.
        message: String,
    },
}

/// Counters since start. All counts are cumulative.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WatchStats {
    /// Raw events received from the backend (errors not included).
    pub raw_events: u64,
    /// `Changes` events delivered.
    pub batches: u64,
    /// `FsChange` entries delivered.
    pub changes: u64,
    /// Raw path events merged into an already buffered path, plus removed descendants
    /// folded into a removed directory.
    pub coalesced: u64,
    /// Paths dropped by the built-in temp filter or the caller's ignore predicate.
    pub ignored: u64,
    /// Paths dropped because they are outside the root.
    pub outside_root: u64,
    /// Windows that lost events: `max_pending` exceeded, raw queue full, or kernel overflow.
    pub overflows: u64,
    /// `Rescan` events delivered.
    pub rescans: u64,
    /// Backend errors.
    pub errors: u64,
    /// Panics caught from the `on_event` callback.
    pub callback_panics: u64,
}

#[derive(Debug, Default)]
pub(crate) struct Counters {
    pub(crate) raw_events: AtomicU64,
    pub(crate) batches: AtomicU64,
    pub(crate) changes: AtomicU64,
    pub(crate) coalesced: AtomicU64,
    pub(crate) ignored: AtomicU64,
    pub(crate) outside_root: AtomicU64,
    pub(crate) overflows: AtomicU64,
    pub(crate) rescans: AtomicU64,
    pub(crate) errors: AtomicU64,
    pub(crate) callback_panics: AtomicU64,
}

impl Counters {
    fn snapshot(&self) -> WatchStats {
        let get = |c: &AtomicU64| c.load(Ordering::Relaxed);
        WatchStats {
            raw_events: get(&self.raw_events),
            batches: get(&self.batches),
            changes: get(&self.changes),
            coalesced: get(&self.coalesced),
            ignored: get(&self.ignored),
            outside_root: get(&self.outside_root),
            overflows: get(&self.overflows),
            rescans: get(&self.rescans),
            errors: get(&self.errors),
            callback_panics: get(&self.callback_panics),
        }
    }
}

/// Errors from [`Watcher::start`].
#[derive(Debug, thiserror::Error)]
pub enum WatchError {
    /// The configuration is unusable.
    #[error("invalid watch configuration: {0}")]
    InvalidConfig(String),
    /// The root could not be canonicalised.
    #[error("cannot resolve workspace root {path:?}: {source}")]
    Root {
        /// The configured root.
        path: PathBuf,
        /// The underlying error.
        #[source]
        source: io::Error,
    },
    /// The root is not a directory.
    #[error("workspace root {path:?} is not a directory")]
    NotADirectory {
        /// The canonical root.
        path: PathBuf,
    },
    /// The inotify watch limit was reached while setting up the recursive watch.
    #[error(
        "the inotify watch limit was reached while watching {path:?}; raise fs.inotify.max_user_watches"
    )]
    WatchLimit {
        /// The canonical root.
        path: PathBuf,
    },
    /// The backend failed to start or to watch the root.
    #[error("file watching backend failed: {0}")]
    Backend(String),
    /// The aggregator thread could not be spawned.
    #[error("cannot spawn the watcher thread: {0}")]
    Spawn(#[source] io::Error),
}

enum Msg {
    Raw(notify::Result<notify::Event>),
    Wake,
}

struct Shared {
    stop: AtomicBool,
    raw_overflow: AtomicBool,
    counters: Arc<Counters>,
}

/// A running workspace watcher. Dropping it stops it like [`Watcher::stop`].
pub struct Watcher {
    root: PathBuf,
    shared: Arc<Shared>,
    wake: SyncSender<Msg>,
    backend: Option<RecommendedWatcher>,
    thread: Option<JoinHandle<()>>,
}

impl fmt::Debug for Watcher {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Watcher")
            .field("root", &self.root)
            .field("stats", &self.stats())
            .finish_non_exhaustive()
    }
}

impl Watcher {
    /// Starts watching `config.root` recursively. `on_event` is called from the watcher's
    /// own thread, one event at a time.
    pub fn start<F>(config: WatchConfig, on_event: F) -> Result<Watcher, WatchError>
    where
        F: FnMut(WatchEvent) + Send + 'static,
    {
        if config.max_pending == 0 {
            return Err(WatchError::InvalidConfig(
                "max_pending must be at least 1".to_string(),
            ));
        }
        let root = std::fs::canonicalize(&config.root).map_err(|source| WatchError::Root {
            path: config.root.clone(),
            source,
        })?;
        if !root.is_dir() {
            return Err(WatchError::NotADirectory { path: root });
        }

        let counters = Arc::new(Counters::default());
        let shared = Arc::new(Shared {
            stop: AtomicBool::new(false),
            raw_overflow: AtomicBool::new(false),
            counters: counters.clone(),
        });
        let (tx, rx) = mpsc::sync_channel::<Msg>(RAW_QUEUE_CAPACITY);

        let backend_tx = tx.clone();
        let backend_shared = shared.clone();
        let handler = move |result: notify::Result<notify::Event>| {
            if backend_shared.stop.load(Ordering::Acquire) {
                return;
            }
            match backend_tx.try_send(Msg::Raw(result)) {
                Ok(()) | Err(TrySendError::Disconnected(_)) => {}
                Err(TrySendError::Full(_)) => {
                    backend_shared.raw_overflow.store(true, Ordering::Release);
                }
            }
        };
        let notify_config = notify::Config::default().with_follow_symlinks(false);
        let mut backend = RecommendedWatcher::new(handler, notify_config)
            .map_err(|e| WatchError::Backend(e.to_string()))?;
        if let Err(error) = backend.watch(&root, RecursiveMode::Recursive) {
            return Err(match error.kind {
                notify::ErrorKind::MaxFilesWatch => WatchError::WatchLimit { path: root },
                _ => WatchError::Backend(error.to_string()),
            });
        }

        let coalescer = Coalescer::new(root.clone(), config.max_pending, config.ignore, counters);
        let thread_shared = shared.clone();
        let debounce = config.debounce;
        let thread = thread::Builder::new()
            .name("titan-watch".to_string())
            .spawn(move || aggregate(&rx, &thread_shared, coalescer, debounce, on_event))
            .map_err(WatchError::Spawn)?;

        Ok(Watcher {
            root,
            shared,
            wake: tx,
            backend: Some(backend),
            thread: Some(thread),
        })
    }

    /// The canonical workspace root; every reported path starts with it.
    pub fn root(&self) -> &Path {
        &self.root
    }

    /// A snapshot of the counters.
    pub fn stats(&self) -> WatchStats {
        self.shared.counters.snapshot()
    }

    /// Stops the backend and joins the aggregator thread. Buffered changes of the current
    /// window are discarded; `on_event` is not called after this returns. Returns within
    /// about 100 ms unless a callback invocation is still running.
    ///
    /// The FSEvents backend joins its run-loop thread on drop. notify's inotify backend
    /// thread is told to shut down and exits on its own shortly after (notify does not
    /// expose a join for it); it no longer delivers anything to this watcher.
    pub fn stop(mut self) {
        self.shutdown();
    }

    fn shutdown(&mut self) {
        self.shared.stop.store(true, Ordering::Release);
        drop(self.backend.take());
        let _ = self.wake.try_send(Msg::Wake);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

impl Drop for Watcher {
    fn drop(&mut self) {
        self.shutdown();
    }
}

/// When a window that opened at `start` and last saw an event at `last` closes. `None` if
/// the deadline is not representable (absurdly large debounce): such a window only closes
/// when the process asks for a stop.
fn window_due(
    start: Instant,
    last: Instant,
    debounce: Duration,
    max_window: Duration,
) -> Option<Instant> {
    match (last.checked_add(debounce), start.checked_add(max_window)) {
        (Some(quiet), Some(cap)) => Some(quiet.min(cap)),
        (Some(due), None) | (None, Some(due)) => Some(due),
        (None, None) => None,
    }
}

fn aggregate<F>(
    rx: &mpsc::Receiver<Msg>,
    shared: &Shared,
    mut coalescer: Coalescer,
    debounce: Duration,
    mut on_event: F,
) where
    F: FnMut(WatchEvent),
{
    let counters = &shared.counters;
    let max_window = debounce.saturating_mul(10);
    let mut seq: u64 = 0;
    // (window start, last relevant event)
    let mut window: Option<(Instant, Instant)> = None;

    let mut emit = |event: WatchEvent| {
        let result = panic::catch_unwind(AssertUnwindSafe(|| on_event(event)));
        if result.is_err() {
            counters.callback_panics.fetch_add(1, Ordering::Relaxed);
        }
    };

    loop {
        if shared.stop.load(Ordering::Acquire) {
            return;
        }
        let due = window.and_then(|(start, last)| window_due(start, last, debounce, max_window));
        let wait = match due {
            Some(due) => due.saturating_duration_since(Instant::now()).min(IDLE_POLL),
            None => IDLE_POLL,
        };
        let mut received = false;
        match rx.recv_timeout(wait) {
            Ok(Msg::Raw(result)) => {
                received = true;
                if let Some(message) = coalescer.push(result) {
                    if shared.stop.load(Ordering::Acquire) {
                        return;
                    }
                    emit(WatchEvent::Error { message });
                }
            }
            Ok(Msg::Wake) | Err(RecvTimeoutError::Timeout) => {}
            Err(RecvTimeoutError::Disconnected) => return,
        }
        if shared.stop.load(Ordering::Acquire) {
            return;
        }
        if shared.raw_overflow.swap(false, Ordering::AcqRel) {
            received = true;
            coalescer.overflow(format!(
                "the raw event queue ({RAW_QUEUE_CAPACITY} events) was full; events were dropped"
            ));
        }
        let now = Instant::now();
        if received && coalescer.has_pending() {
            window = Some(match window {
                Some((start, _)) => (start, now),
                None => (now, now),
            });
        }
        if let Some((start, last)) = window {
            let closed =
                window_due(start, last, debounce, max_window).is_some_and(|due| now >= due);
            if closed {
                window = None;
                match coalescer.flush() {
                    Some(Flushed::Changes(changes)) => {
                        seq += 1;
                        counters.batches.fetch_add(1, Ordering::Relaxed);
                        counters
                            .changes
                            .fetch_add(changes.len() as u64, Ordering::Relaxed);
                        emit(WatchEvent::Changes { seq, changes });
                    }
                    Some(Flushed::Rescan(reason)) => {
                        seq += 1;
                        counters.rescans.fetch_add(1, Ordering::Relaxed);
                        emit(WatchEvent::Rescan { seq, reason });
                    }
                    None => {}
                }
            }
        }
    }
}
