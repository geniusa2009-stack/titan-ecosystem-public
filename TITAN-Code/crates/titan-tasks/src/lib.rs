//! TITAN Code task coordinator.
//!
//! Background work (parsing, indexing, scanning, search; later language servers and agents)
//! runs on a fixed set of worker threads fed by a **bounded** queue with three priorities.
//! The latency-critical path (applying edits) never runs here: it stays on the server's
//! request loop and only *submits* work.
//!
//! * **Backpressure:** `submit` fails with [`SubmitError::QueueFull`] instead of queueing
//!   without bound.
//! * **Priorities:** `High` before `Normal` before `Low`; one pick in
//!   [`PoolConfig::low_priority_every`] serves the lowest non-empty priority first so
//!   background work cannot starve forever.
//! * **Cancellation is cooperative:** every task gets a [`CancelToken`]; long jobs poll it.
//!   A task cancelled before it starts never runs. A task whose token is cancelled (or whose
//!   deadline passes) is reported as `Cancelled`/`TimedOut` and its result is discarded.
//! * **Error isolation:** a panicking job is caught and reported as `Panicked`; the worker
//!   keeps serving.
//! * **Completion** is delivered through the `on_done` callback (the server forwards it into
//!   its event loop), off the queue lock.

use std::collections::{HashMap, VecDeque};
use std::panic::{self, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, PoisonError};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Priority {
    High,
    Normal,
    Low,
}

impl Priority {
    fn index(self) -> usize {
        match self {
            Self::High => 0,
            Self::Normal => 1,
            Self::Low => 2,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Self::High => "high",
            Self::Normal => "normal",
            Self::Low => "low",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct TaskId(pub u64);

#[derive(Debug)]
struct TokenInner {
    cancelled: AtomicBool,
    deadline: Option<Instant>,
}

/// Shared cancellation flag plus an optional deadline. Cheap to clone and to poll.
#[derive(Clone, Debug)]
pub struct CancelToken {
    inner: Arc<TokenInner>,
}

impl CancelToken {
    pub fn new(deadline: Option<Instant>) -> Self {
        Self {
            inner: Arc::new(TokenInner {
                cancelled: AtomicBool::new(false),
                deadline,
            }),
        }
    }

    pub fn cancel(&self) {
        self.inner.cancelled.store(true, Ordering::Release);
    }

    /// True if explicitly cancelled.
    pub fn was_cancelled(&self) -> bool {
        self.inner.cancelled.load(Ordering::Acquire)
    }

    /// True if the deadline has passed.
    pub fn is_expired(&self) -> bool {
        self.inner
            .deadline
            .is_some_and(|deadline| Instant::now() >= deadline)
    }

    /// True if the job should stop: cancelled or past its deadline.
    pub fn is_cancelled(&self) -> bool {
        self.was_cancelled() || self.is_expired()
    }
}

#[derive(Debug)]
pub enum TaskOutcome<R> {
    Completed(R),
    Cancelled,
    TimedOut,
    Panicked(String),
}

#[derive(Debug)]
pub struct TaskReport<R> {
    pub id: TaskId,
    pub name: &'static str,
    pub priority: Priority,
    pub outcome: TaskOutcome<R>,
    /// Time spent queued before a worker picked the task up.
    pub queued: Duration,
    /// Time the job ran (zero if it never started).
    pub ran: Duration,
}

#[derive(Clone, Copy, Debug)]
pub struct PoolConfig {
    pub workers: usize,
    /// Maximum number of queued (not yet running) tasks.
    pub queue_capacity: usize,
    /// Every N-th pick serves the lowest non-empty priority first (0 disables).
    pub low_priority_every: u32,
}

impl Default for PoolConfig {
    fn default() -> Self {
        Self {
            workers: 2,
            queue_capacity: 1024,
            low_priority_every: 8,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, thiserror::Error)]
pub enum SubmitError {
    #[error("the task queue is full ({capacity} tasks)")]
    QueueFull { capacity: usize },
    #[error("the task pool is shutting down")]
    ShuttingDown,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShutdownMode {
    /// Finish everything already queued.
    Drain,
    /// Cancel running tasks and report queued ones as cancelled without running them.
    Cancel,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PoolMetrics {
    pub submitted: u64,
    pub completed: u64,
    pub cancelled: u64,
    pub timed_out: u64,
    pub panicked: u64,
    pub rejected: u64,
    pub queued: usize,
    pub running: usize,
    pub max_queue_depth: usize,
    pub total_queue_wait_us: u64,
    pub total_run_us: u64,
}

type Job<R> = Box<dyn FnOnce(&CancelToken) -> R + Send + 'static>;

struct QueuedTask<R> {
    id: TaskId,
    name: &'static str,
    priority: Priority,
    token: CancelToken,
    job: Job<R>,
    enqueued: Instant,
}

struct State<R> {
    queues: [VecDeque<QueuedTask<R>>; 3],
    tokens: HashMap<TaskId, CancelToken>,
    shutting_down: bool,
    picks_until_low: u32,
    metrics: PoolMetrics,
}

impl<R> State<R> {
    fn queued(&self) -> usize {
        self.queues.iter().map(VecDeque::len).sum()
    }

    fn pop_next(&mut self, low_every: u32) -> Option<QueuedTask<R>> {
        if low_every > 0 {
            self.picks_until_low = self.picks_until_low.saturating_sub(1);
            if self.picks_until_low == 0 {
                self.picks_until_low = low_every;
                if let Some(task) = self.queues.iter_mut().rev().find_map(VecDeque::pop_front) {
                    return Some(task);
                }
            }
        }
        self.queues.iter_mut().find_map(VecDeque::pop_front)
    }
}

type DoneCallback<R> = Box<dyn Fn(TaskReport<R>) + Send + Sync>;

struct Shared<R> {
    state: Mutex<State<R>>,
    available: Condvar,
    idle: Condvar,
    on_done: DoneCallback<R>,
    config: PoolConfig,
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(PoisonError::into_inner)
}

fn duration_us(d: Duration) -> u64 {
    u64::try_from(d.as_micros()).unwrap_or(u64::MAX)
}

fn panic_message(payload: &(dyn std::any::Any + Send)) -> String {
    if let Some(s) = payload.downcast_ref::<&str>() {
        (*s).to_string()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        "panic with a non-string payload".to_string()
    }
}

fn stopped_outcome<R>(token: &CancelToken) -> TaskOutcome<R> {
    if token.was_cancelled() {
        TaskOutcome::Cancelled
    } else {
        TaskOutcome::TimedOut
    }
}

fn worker_loop<R: Send + 'static>(shared: &Shared<R>) {
    loop {
        let task = {
            let mut state = lock(&shared.state);
            loop {
                if let Some(task) = state.pop_next(shared.config.low_priority_every) {
                    state.metrics.running += 1;
                    break Some(task);
                }
                if state.shutting_down {
                    break None;
                }
                state = shared
                    .available
                    .wait(state)
                    .unwrap_or_else(PoisonError::into_inner);
            }
        };
        let Some(QueuedTask {
            id,
            name,
            priority,
            token,
            job,
            enqueued,
        }) = task
        else {
            return;
        };
        let queued = enqueued.elapsed();
        let started = Instant::now();
        let outcome = if token.is_cancelled() {
            stopped_outcome(&token)
        } else {
            match panic::catch_unwind(AssertUnwindSafe(|| job(&token))) {
                Ok(_) if token.is_cancelled() => stopped_outcome(&token),
                Ok(value) => TaskOutcome::Completed(value),
                Err(payload) => TaskOutcome::Panicked(panic_message(payload.as_ref())),
            }
        };
        let ran = started.elapsed();
        let kind = match &outcome {
            TaskOutcome::Completed(_) => 0,
            TaskOutcome::Cancelled => 1,
            TaskOutcome::TimedOut => 2,
            TaskOutcome::Panicked(_) => 3,
        };
        (shared.on_done)(TaskReport {
            id,
            name,
            priority,
            outcome,
            queued,
            ran,
        });
        let mut state = lock(&shared.state);
        state.tokens.remove(&id);
        state.metrics.running -= 1;
        match kind {
            0 => state.metrics.completed += 1,
            1 => state.metrics.cancelled += 1,
            2 => state.metrics.timed_out += 1,
            _ => state.metrics.panicked += 1,
        }
        state.metrics.total_queue_wait_us += duration_us(queued);
        state.metrics.total_run_us += duration_us(ran);
        drop(state);
        shared.idle.notify_all();
    }
}

/// A fixed pool of worker threads executing jobs that produce `R`.
pub struct TaskPool<R: Send + 'static> {
    shared: Arc<Shared<R>>,
    workers: Vec<JoinHandle<()>>,
    next_id: AtomicU64,
}

impl<R: Send + 'static> std::fmt::Debug for TaskPool<R> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("TaskPool")
            .field("workers", &self.workers.len())
            .field("metrics", &self.metrics())
            .finish()
    }
}

impl<R: Send + 'static> TaskPool<R> {
    pub fn new(
        config: PoolConfig,
        on_done: impl Fn(TaskReport<R>) + Send + Sync + 'static,
    ) -> std::io::Result<Self> {
        let shared = Arc::new(Shared {
            state: Mutex::new(State {
                queues: [VecDeque::new(), VecDeque::new(), VecDeque::new()],
                tokens: HashMap::new(),
                shutting_down: false,
                picks_until_low: config.low_priority_every,
                metrics: PoolMetrics::default(),
            }),
            available: Condvar::new(),
            idle: Condvar::new(),
            on_done: Box::new(on_done),
            config,
        });
        let mut pool = Self {
            shared,
            workers: Vec::new(),
            next_id: AtomicU64::new(1),
        };
        for i in 0..config.workers.max(1) {
            let shared = Arc::clone(&pool.shared);
            let handle = thread::Builder::new()
                .name(format!("titan-task-{i}"))
                .spawn(move || worker_loop(&shared))?;
            pool.workers.push(handle);
        }
        Ok(pool)
    }

    /// Queue a job. `deadline` is measured from now.
    pub fn submit<F>(
        &self,
        name: &'static str,
        priority: Priority,
        deadline: Option<Duration>,
        job: F,
    ) -> Result<(TaskId, CancelToken), SubmitError>
    where
        F: FnOnce(&CancelToken) -> R + Send + 'static,
    {
        let mut state = lock(&self.shared.state);
        if state.shutting_down {
            return Err(SubmitError::ShuttingDown);
        }
        let capacity = self.shared.config.queue_capacity;
        if state.queued() >= capacity {
            state.metrics.rejected += 1;
            return Err(SubmitError::QueueFull { capacity });
        }
        let id = TaskId(self.next_id.fetch_add(1, Ordering::Relaxed));
        let token = CancelToken::new(deadline.map(|d| Instant::now() + d));
        state.tokens.insert(id, token.clone());
        state.queues[priority.index()].push_back(QueuedTask {
            id,
            name,
            priority,
            token: token.clone(),
            job: Box::new(job),
            enqueued: Instant::now(),
        });
        state.metrics.submitted += 1;
        let depth = state.queued();
        state.metrics.max_queue_depth = state.metrics.max_queue_depth.max(depth);
        drop(state);
        self.shared.available.notify_one();
        Ok((id, token))
    }

    /// Cancel a queued or running task. Returns false if it already finished.
    pub fn cancel(&self, id: TaskId) -> bool {
        let state = lock(&self.shared.state);
        match state.tokens.get(&id) {
            Some(token) => {
                token.cancel();
                true
            }
            None => false,
        }
    }

    pub fn cancel_all(&self) {
        let state = lock(&self.shared.state);
        for token in state.tokens.values() {
            token.cancel();
        }
    }

    pub fn metrics(&self) -> PoolMetrics {
        let state = lock(&self.shared.state);
        let mut metrics = state.metrics;
        metrics.queued = state.queued();
        metrics
    }

    /// Block until nothing is queued or running, or `timeout` passes. Returns true if idle.
    pub fn wait_idle(&self, timeout: Duration) -> bool {
        let deadline = Instant::now() + timeout;
        let mut state = lock(&self.shared.state);
        while state.queued() > 0 || state.metrics.running > 0 {
            let now = Instant::now();
            if now >= deadline {
                return false;
            }
            state = self
                .shared
                .idle
                .wait_timeout(state, deadline - now)
                .map(|(guard, _)| guard)
                .unwrap_or_else(|poison| poison.into_inner().0);
        }
        true
    }

    fn stop(&mut self, mode: ShutdownMode) {
        {
            let mut state = lock(&self.shared.state);
            state.shutting_down = true;
            if mode == ShutdownMode::Cancel {
                for token in state.tokens.values() {
                    token.cancel();
                }
            }
        }
        self.shared.available.notify_all();
        for handle in self.workers.drain(..) {
            // A worker only panics if `on_done` panics; nothing more to do then.
            let _ = handle.join();
        }
    }

    /// Stop accepting work, then drain or cancel, and join all workers.
    pub fn shutdown(mut self, mode: ShutdownMode) -> PoolMetrics {
        self.stop(mode);
        self.metrics()
    }
}

impl<R: Send + 'static> Drop for TaskPool<R> {
    fn drop(&mut self) {
        if !self.workers.is_empty() {
            self.stop(ShutdownMode::Cancel);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc;

    type Log = Arc<Mutex<Vec<(&'static str, String)>>>;

    fn outcome_name<R>(outcome: &TaskOutcome<R>) -> String {
        match outcome {
            TaskOutcome::Completed(_) => "completed".to_string(),
            TaskOutcome::Cancelled => "cancelled".to_string(),
            TaskOutcome::TimedOut => "timed_out".to_string(),
            TaskOutcome::Panicked(msg) => format!("panicked: {msg}"),
        }
    }

    fn pool(workers: usize, capacity: usize) -> (TaskPool<usize>, Log) {
        let log: Log = Arc::new(Mutex::new(Vec::new()));
        let sink = Arc::clone(&log);
        let config = PoolConfig {
            workers,
            queue_capacity: capacity,
            low_priority_every: 0,
        };
        let pool = TaskPool::new(config, move |report: TaskReport<usize>| {
            lock(&sink).push((report.name, outcome_name(&report.outcome)));
        })
        .unwrap();
        (pool, log)
    }

    /// Submit a job that signals when it starts and then waits for `release`.
    fn blocker(pool: &TaskPool<usize>) -> mpsc::Sender<()> {
        let (started_tx, started_rx) = mpsc::channel();
        let (release_tx, release_rx) = mpsc::channel::<()>();
        pool.submit("blocker", Priority::Normal, None, move |token| {
            started_tx.send(()).unwrap();
            while release_rx.recv_timeout(Duration::from_millis(5)).is_err() {
                if token.is_cancelled() {
                    break;
                }
            }
            0
        })
        .unwrap();
        started_rx.recv_timeout(Duration::from_secs(10)).unwrap();
        release_tx
    }

    #[test]
    fn every_task_completes_exactly_once() {
        let results = Arc::new(Mutex::new(Vec::new()));
        let sink = Arc::clone(&results);
        let pool = TaskPool::new(
            PoolConfig {
                workers: 4,
                queue_capacity: 10_000,
                low_priority_every: 8,
            },
            move |report: TaskReport<usize>| {
                if let TaskOutcome::Completed(v) = report.outcome {
                    lock(&sink).push(v);
                }
            },
        )
        .unwrap();
        let priorities = [Priority::High, Priority::Normal, Priority::Low];
        for i in 0..1_000 {
            pool.submit("n", priorities[i % 3], None, move |_| i)
                .unwrap();
        }
        assert!(pool.wait_idle(Duration::from_secs(30)));
        let mut got = lock(&results).clone();
        got.sort_unstable();
        assert_eq!(got, (0..1_000).collect::<Vec<_>>());
        let metrics = pool.shutdown(ShutdownMode::Drain);
        assert_eq!(metrics.submitted, 1_000);
        assert_eq!(metrics.completed, 1_000);
        assert_eq!(metrics.running, 0);
        assert_eq!(metrics.queued, 0);
    }

    #[test]
    fn higher_priority_runs_first() {
        let (pool, log) = pool(1, 16);
        let release = blocker(&pool);
        pool.submit("low", Priority::Low, None, |_| 0).unwrap();
        pool.submit("normal", Priority::Normal, None, |_| 0)
            .unwrap();
        pool.submit("high", Priority::High, None, |_| 0).unwrap();
        release.send(()).unwrap();
        assert!(pool.wait_idle(Duration::from_secs(10)));
        let order: Vec<&str> = lock(&log).iter().map(|(name, _)| *name).collect();
        assert_eq!(order, vec!["blocker", "high", "normal", "low"]);
    }

    #[test]
    fn low_priority_is_not_starved() {
        let log: Log = Arc::new(Mutex::new(Vec::new()));
        let sink = Arc::clone(&log);
        let pool = TaskPool::new(
            PoolConfig {
                workers: 1,
                queue_capacity: 64,
                low_priority_every: 3,
            },
            move |report: TaskReport<usize>| {
                lock(&sink).push((report.name, outcome_name(&report.outcome)));
            },
        )
        .unwrap();
        let release = blocker(&pool);
        pool.submit("low", Priority::Low, None, |_| 0).unwrap();
        for _ in 0..10 {
            pool.submit("high", Priority::High, None, |_| 0).unwrap();
        }
        release.send(()).unwrap();
        assert!(pool.wait_idle(Duration::from_secs(10)));
        let order: Vec<&str> = lock(&log).iter().map(|(name, _)| *name).collect();
        let low_at = order.iter().position(|n| *n == "low").unwrap();
        assert!(low_at <= 3, "low ran at position {low_at}: {order:?}");
    }

    #[test]
    fn full_queue_rejects_and_counts() {
        let (pool, _log) = pool(1, 2);
        let release = blocker(&pool);
        pool.submit("a", Priority::Normal, None, |_| 0).unwrap();
        pool.submit("b", Priority::Normal, None, |_| 0).unwrap();
        assert_eq!(
            pool.submit("c", Priority::High, None, |_| 0).unwrap_err(),
            SubmitError::QueueFull { capacity: 2 }
        );
        release.send(()).unwrap();
        assert!(pool.wait_idle(Duration::from_secs(10)));
        assert_eq!(pool.metrics().rejected, 1);
        assert_eq!(pool.metrics().completed, 3);
    }

    #[test]
    fn cancelled_before_start_never_runs() {
        let (pool, log) = pool(1, 8);
        let release = blocker(&pool);
        let ran = Arc::new(AtomicBool::new(false));
        let flag = Arc::clone(&ran);
        let (id, _) = pool
            .submit("victim", Priority::High, None, move |_| {
                flag.store(true, Ordering::SeqCst);
                0
            })
            .unwrap();
        assert!(pool.cancel(id));
        release.send(()).unwrap();
        assert!(pool.wait_idle(Duration::from_secs(10)));
        assert!(!ran.load(Ordering::SeqCst));
        assert!(
            lock(&log).contains(&("victim", "cancelled".to_string())),
            "{:?}",
            lock(&log)
        );
        assert!(!pool.cancel(id), "finished tasks cannot be cancelled");
    }

    #[test]
    fn running_task_observes_cancellation() {
        let (pool, log) = pool(1, 8);
        let (started_tx, started_rx) = mpsc::channel();
        let (id, _) = pool
            .submit("spin", Priority::Normal, None, move |token| {
                started_tx.send(()).unwrap();
                let begin = Instant::now();
                while !token.is_cancelled() && begin.elapsed() < Duration::from_secs(10) {
                    thread::sleep(Duration::from_millis(1));
                }
                7
            })
            .unwrap();
        started_rx.recv_timeout(Duration::from_secs(10)).unwrap();
        pool.cancel(id);
        assert!(pool.wait_idle(Duration::from_secs(10)));
        assert_eq!(lock(&log)[0], ("spin", "cancelled".to_string()));
    }

    #[test]
    fn deadline_produces_timeout() {
        let (pool, log) = pool(1, 8);
        pool.submit(
            "slow",
            Priority::Normal,
            Some(Duration::from_millis(30)),
            |token| {
                let begin = Instant::now();
                while !token.is_cancelled() && begin.elapsed() < Duration::from_secs(10) {
                    thread::sleep(Duration::from_millis(1));
                }
                0
            },
        )
        .unwrap();
        assert!(pool.wait_idle(Duration::from_secs(10)));
        assert_eq!(lock(&log)[0], ("slow", "timed_out".to_string()));
        assert_eq!(pool.metrics().timed_out, 1);
    }

    #[test]
    fn panics_are_isolated() {
        let (pool, log) = pool(1, 8);
        pool.submit("boom", Priority::Normal, None, |_| panic!("boom"))
            .unwrap();
        pool.submit("after", Priority::Normal, None, |_| 1).unwrap();
        assert!(pool.wait_idle(Duration::from_secs(10)));
        let entries = lock(&log).clone();
        assert_eq!(entries[0], ("boom", "panicked: boom".to_string()));
        assert_eq!(entries[1], ("after", "completed".to_string()));
        assert_eq!(pool.metrics().panicked, 1);
    }

    #[test]
    fn cancel_shutdown_reports_every_queued_task() {
        let (pool, log) = pool(1, 8);
        let _release = blocker(&pool);
        for _ in 0..3 {
            pool.submit("queued", Priority::Low, None, |_| 0).unwrap();
        }
        let metrics = pool.shutdown(ShutdownMode::Cancel);
        assert_eq!(metrics.cancelled, 4, "blocker plus three queued");
        assert_eq!(lock(&log).len(), 4);
        assert_eq!(metrics.running, 0);
    }
}
