//! Controlled execution of one program: fixed argv (never a shell), empty environment plus
//! an explicit allow list, own process group, wall-clock timeout, bounded output capture.
//!
//! On Unix the child is spawned as the leader of a new process group
//! (`CommandExt::process_group(0)`). On timeout the whole group is killed with `SIGKILL`
//! *before* the leader is reaped, so the group id cannot have been reused. After the leader
//! exits by itself the group is killed once more (best effort) to remove stragglers such as
//! background processes started by tests; a process that left the group (`setsid`,
//! `setpgid`) is not reached — see the ADR for the isolation boundary.

use std::ffi::OsString;
use std::io::{self, Read};
use std::path::Path;
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::mpsc::{self, RecvTimeoutError, Sender};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::thread;
use std::time::{Duration, Instant};

use crate::libtest::{LibtestParser, TestSummary};
use crate::log::{BoundedLog, LogCapture, LogLimits};

/// How long the output readers may take to drain the pipes after the process ended.
const READER_GRACE: Duration = Duration::from_secs(2);
/// Poll interval of the exit/timeout loop.
const POLL: Duration = Duration::from_millis(10);

pub(crate) struct ProcessSpec<'a> {
    pub program: &'a Path,
    pub args: &'a [OsString],
    pub cwd: &'a Path,
    pub env: &'a [(OsString, OsString)],
    pub timeout: Duration,
    pub log_limits: LogLimits,
    /// Feed stdout to the libtest parser.
    pub parse_tests: bool,
    /// Also collect stdout verbatim, up to this many bytes (0 = do not collect).
    pub stdout_limit: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Exit {
    /// Exited by itself with this status.
    Exited {
        code: Option<i32>,
        signal: Option<i32>,
    },
    /// Killed by the pipeline after the timeout.
    TimedOut,
}

#[derive(Debug)]
pub(crate) struct ProcessOutcome {
    pub exit: Exit,
    pub duration: Duration,
    pub log: BoundedLog,
    pub tests: Option<TestSummary>,
    pub stdout: Vec<u8>,
    pub stdout_overflow: bool,
}

impl ProcessOutcome {
    pub(crate) fn success(&self) -> bool {
        matches!(self.exit, Exit::Exited { code: Some(0), .. })
    }

    /// The exit code, if the process exited normally.
    pub(crate) fn code(&self) -> Option<i32> {
        match self.exit {
            Exit::Exited { code, .. } => code,
            Exit::TimedOut => None,
        }
    }

    /// "exit status 1", "signal 9", "timed out" — for error messages.
    pub(crate) fn describe(&self) -> String {
        match self.exit {
            Exit::Exited {
                code: Some(code), ..
            } => format!("exit status {code}"),
            Exit::Exited {
                signal: Some(sig), ..
            } => format!("killed by signal {sig}"),
            Exit::Exited { .. } => "unknown exit status".to_string(),
            Exit::TimedOut => "timed out".to_string(),
        }
    }
}

struct Shared {
    log: LogCapture,
    parser: Option<LibtestParser>,
    stdout: Vec<u8>,
    stdout_limit: usize,
    stdout_overflow: bool,
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Runs `spec` to completion or timeout. `Err` only when the process could not be started.
pub(crate) fn run(spec: &ProcessSpec<'_>) -> io::Result<ProcessOutcome> {
    let mut cmd = Command::new(spec.program);
    cmd.args(spec.args)
        .current_dir(spec.cwd)
        .env_clear()
        .envs(spec.env.iter().map(|(k, v)| (k, v)))
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;
        cmd.process_group(0);
    }
    let start = Instant::now();
    let mut child = cmd.spawn()?;
    let shared = Arc::new(Mutex::new(Shared {
        log: LogCapture::new(spec.log_limits),
        parser: spec.parse_tests.then(LibtestParser::default),
        stdout: Vec::new(),
        stdout_limit: spec.stdout_limit,
        stdout_overflow: false,
    }));
    let (done_tx, done_rx) = mpsc::channel::<()>();
    if let Err(e) = start_readers(&mut child, &shared, &done_tx) {
        kill_group(&mut child);
        let _ = child.wait();
        return Err(e);
    }
    drop(done_tx);

    let exit = loop {
        match child.try_wait() {
            Ok(Some(status)) => break exited(status),
            Ok(None) if start.elapsed() < spec.timeout => thread::sleep(POLL),
            Ok(None) => {
                // The leader is not reaped yet, so its pid still names our group.
                kill_group(&mut child);
                let _ = child.wait();
                break Exit::TimedOut;
            }
            Err(_) => {
                kill_group(&mut child);
                let status = child.wait()?;
                break exited(status);
            }
        }
    };
    let duration = start.elapsed();
    // Stragglers (e.g. a background process started by a test) still hold the group id.
    kill_group_after_exit(child.id());

    let deadline = Instant::now() + READER_GRACE;
    let mut readers_finished = true;
    loop {
        let left = deadline.saturating_duration_since(Instant::now());
        match done_rx.recv_timeout(left) {
            Ok(()) => {}
            Err(RecvTimeoutError::Disconnected) => break,
            Err(RecvTimeoutError::Timeout) => {
                readers_finished = false;
                break;
            }
        }
    }
    let mut state = lock(&shared);
    if !readers_finished {
        state.log.push(
            b"\n[titan-verify: output still open after the process ended; capture stopped]\n",
        );
    }
    let log = std::mem::replace(&mut state.log, LogCapture::new(spec.log_limits)).finish();
    let tests = state.parser.take().and_then(LibtestParser::finish);
    Ok(ProcessOutcome {
        exit,
        duration,
        log,
        tests,
        stdout: std::mem::take(&mut state.stdout),
        stdout_overflow: state.stdout_overflow,
    })
}

fn exited(status: ExitStatus) -> Exit {
    #[cfg(unix)]
    let signal = {
        use std::os::unix::process::ExitStatusExt;
        status.signal()
    };
    #[cfg(not(unix))]
    let signal = None;
    Exit::Exited {
        code: status.code(),
        signal,
    }
}

fn start_readers(
    child: &mut Child,
    shared: &Arc<Mutex<Shared>>,
    done: &Sender<()>,
) -> io::Result<()> {
    let (Some(stdout), Some(stderr)) = (child.stdout.take(), child.stderr.take()) else {
        return Err(io::Error::other("child pipes were not created"));
    };
    for (name, is_stdout, pipe) in [
        (
            "verify-stdout",
            true,
            Box::new(stdout) as Box<dyn Read + Send>,
        ),
        ("verify-stderr", false, Box::new(stderr)),
    ] {
        let shared = Arc::clone(shared);
        let done = done.clone();
        thread::Builder::new()
            .name(name.to_string())
            .spawn(move || {
                read_pipe(pipe, is_stdout, &shared);
                drop(done);
            })?;
    }
    Ok(())
}

fn read_pipe(mut pipe: Box<dyn Read + Send>, is_stdout: bool, shared: &Mutex<Shared>) {
    let mut buf = vec![0u8; 16 * 1024];
    loop {
        match pipe.read(&mut buf) {
            Ok(0) => return,
            Ok(n) => {
                let chunk = &buf[..n];
                let mut state = lock(shared);
                state.log.push(chunk);
                if is_stdout {
                    if let Some(parser) = state.parser.as_mut() {
                        parser.feed(chunk);
                    }
                    if state.stdout_limit > 0 {
                        if state.stdout.len() + n <= state.stdout_limit {
                            state.stdout.extend_from_slice(chunk);
                        } else {
                            state.stdout_overflow = true;
                        }
                    }
                }
            }
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(_) => return,
        }
    }
}

/// Kills the child's whole process group (Unix) or the child (elsewhere). The leader must
/// not have been reaped yet.
fn kill_group(child: &mut Child) {
    #[cfg(unix)]
    {
        if let Some(pid) = i32::try_from(child.id())
            .ok()
            .and_then(rustix::process::Pid::from_raw)
        {
            let _ = rustix::process::kill_process_group(pid, rustix::process::Signal::KILL);
        }
    }
    let _ = child.kill();
}

/// Best-effort kill of what is left of the group after the leader was reaped.
fn kill_group_after_exit(pid: u32) {
    #[cfg(unix)]
    {
        if let Some(pid) = i32::try_from(pid)
            .ok()
            .and_then(rustix::process::Pid::from_raw)
        {
            let _ = rustix::process::kill_process_group(pid, rustix::process::Signal::KILL);
        }
    }
    #[cfg(not(unix))]
    let _ = pid;
}
