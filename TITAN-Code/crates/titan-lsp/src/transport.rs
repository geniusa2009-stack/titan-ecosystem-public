//! One server process and its three I/O threads.
//!
//! * reader: `stdout` -> framed messages -> parsed JSON -> control channel. EOF, a framing
//!   error, an oversize message or malformed JSON ends the thread with a `Lost` event.
//! * writer: outgoing queue -> one framed message per write -> `stdin`. A write error ends
//!   the thread with a `Lost` event; dropping every queue sender closes `stdin`.
//! * stderr: drained continuously (so the server never blocks on a full pipe) into a
//!   bounded ring buffer used for crash reports.
//!
//! The child is killed and reaped when the [`Connection`] is dropped.

use std::collections::VecDeque;
use std::io::{self, BufReader, BufWriter, Read};
use std::process::{Child, ChildStderr, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender, SyncSender};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::thread;
use std::time::{Duration, Instant};

use serde_json::Value;
use titan_protocol::framing::{self, FramingError};

use crate::config::LspCommand;
use crate::supervisor::Msg;

/// Events from the I/O threads of one generation.
#[derive(Debug)]
pub(crate) enum ConnEvent {
    Incoming {
        generation: u64,
        message: Value,
        bytes: usize,
    },
    Lost {
        generation: u64,
        reason: String,
    },
}

pub(crate) fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Bounded buffer keeping the most recent bytes written to it.
#[derive(Debug)]
pub(crate) struct StderrTail {
    buf: VecDeque<u8>,
    cap: usize,
}

impl StderrTail {
    pub(crate) fn new(cap: usize) -> Self {
        Self {
            buf: VecDeque::with_capacity(cap.min(64 * 1024)),
            cap,
        }
    }

    pub(crate) fn push(&mut self, data: &[u8]) {
        if data.len() >= self.cap {
            self.buf.clear();
            self.buf.extend(&data[data.len() - self.cap..]);
            return;
        }
        let overflow = (self.buf.len() + data.len()).saturating_sub(self.cap);
        self.buf.drain(..overflow);
        self.buf.extend(data);
    }

    pub(crate) fn text(&self) -> String {
        let (front, back) = self.buf.as_slices();
        let mut bytes = Vec::with_capacity(self.buf.len());
        bytes.extend_from_slice(front);
        bytes.extend_from_slice(back);
        String::from_utf8_lossy(&bytes).into_owned()
    }
}

#[derive(Debug)]
pub(crate) struct SpawnFailure {
    /// The program cannot be started at all (missing, not executable): retrying is pointless.
    pub permanent: bool,
    pub message: String,
}

/// How the process ended.
#[derive(Debug)]
pub(crate) struct ExitInfo {
    pub description: String,
    pub killed: bool,
}

pub(crate) struct Connection {
    child: Child,
    pid: u32,
    outgoing: Option<SyncSender<Vec<u8>>>,
    stderr: Arc<Mutex<StderrTail>>,
    stderr_done: Receiver<()>,
    exited: bool,
}

impl Connection {
    pub(crate) fn spawn(
        command: &LspCommand,
        generation: u64,
        control: &Sender<Msg>,
        max_message_bytes: usize,
        queue_capacity: usize,
        stderr_tail_bytes: usize,
    ) -> Result<Self, SpawnFailure> {
        let mut cmd = Command::new(&command.program);
        cmd.args(&command.args)
            .current_dir(&command.cwd)
            .envs(command.env.iter().map(|(k, v)| (k.as_str(), v.as_str())))
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let mut child = cmd.spawn().map_err(|e| SpawnFailure {
            permanent: matches!(
                e.kind(),
                io::ErrorKind::NotFound | io::ErrorKind::PermissionDenied
            ),
            message: format!(
                "cannot start language server {} (cwd {}): {e}",
                command.program.display(),
                command.cwd.display()
            ),
        })?;
        let pid = child.id();
        let stdin = child.stdin.take();
        let stdout = child.stdout.take();
        let stderr = child.stderr.take();
        let (outgoing, outgoing_rx) = mpsc::sync_channel::<Vec<u8>>(queue_capacity.max(1));
        let (stderr_done_tx, stderr_done) = mpsc::channel::<()>();
        let tail = Arc::new(Mutex::new(StderrTail::new(stderr_tail_bytes)));
        // From here on, dropping `connection` kills and reaps the child.
        let connection = Self {
            child,
            pid,
            outgoing: Some(outgoing),
            stderr: Arc::clone(&tail),
            stderr_done,
            exited: false,
        };
        let (Some(stdin), Some(stdout), Some(stderr)) = (stdin, stdout, stderr) else {
            return Err(SpawnFailure {
                permanent: false,
                message: "language server pipes were not created".to_string(),
            });
        };
        let thread_error = |e: io::Error| SpawnFailure {
            permanent: false,
            message: format!("cannot start an I/O thread for the language server: {e}"),
        };
        let reader_control = control.clone();
        thread::Builder::new()
            .name(format!("lsp-read-{generation}"))
            .spawn(move || {
                reader_loop(stdout, generation, max_message_bytes, &reader_control);
            })
            .map_err(thread_error)?;
        let writer_control = control.clone();
        thread::Builder::new()
            .name(format!("lsp-write-{generation}"))
            .spawn(move || writer_loop(stdin, generation, &outgoing_rx, &writer_control))
            .map_err(thread_error)?;
        thread::Builder::new()
            .name(format!("lsp-stderr-{generation}"))
            .spawn(move || {
                stderr_loop(stderr, &tail);
                drop(stderr_done_tx);
            })
            .map_err(thread_error)?;
        Ok(connection)
    }

    pub(crate) fn pid(&self) -> u32 {
        self.pid
    }

    /// A handle for queueing outgoing messages, `None` after [`Self::close_input`].
    pub(crate) fn sender(&self) -> Option<SyncSender<Vec<u8>>> {
        self.outgoing.clone()
    }

    /// Drops this connection's queue sender; once every other clone is gone, the writer
    /// drains what is queued and closes the server's `stdin`.
    pub(crate) fn close_input(&mut self) {
        self.outgoing = None;
    }

    /// True once the process has exited (it is reaped; the status is kept for [`Self::finish`]).
    pub(crate) fn has_exited(&mut self) -> bool {
        matches!(self.child.try_wait(), Ok(Some(_)))
    }

    /// Waits up to `grace` for the process to exit by itself, then kills it, and reaps it.
    pub(crate) fn finish(&mut self, grace: Duration) -> ExitInfo {
        let deadline = Instant::now() + grace;
        loop {
            match self.child.try_wait() {
                Ok(Some(status)) => {
                    self.exited = true;
                    return ExitInfo {
                        description: status.to_string(),
                        killed: false,
                    };
                }
                Ok(None) if Instant::now() < deadline => thread::sleep(Duration::from_millis(5)),
                Ok(None) | Err(_) => break,
            }
        }
        let _ = self.child.kill();
        let description = match self.child.wait() {
            Ok(status) => format!("killed; {status}"),
            Err(e) => format!("killed; wait failed: {e}"),
        };
        self.exited = true;
        ExitInfo {
            description,
            killed: true,
        }
    }

    /// The last bytes the server wrote to stderr. Waits briefly for the stderr thread to
    /// drain what a dead process left in the pipe.
    pub(crate) fn stderr_tail(&self) -> String {
        // Disconnected = the thread drained everything; Timeout = a live process (or a
        // grandchild holding the pipe): report what we have.
        let _: Result<(), RecvTimeoutError> =
            self.stderr_done.recv_timeout(Duration::from_millis(500));
        lock(&self.stderr).text()
    }
}

impl Drop for Connection {
    fn drop(&mut self) {
        if !self.exited {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }
}

fn reader_loop(
    stdout: ChildStdout,
    generation: u64,
    max_message_bytes: usize,
    control: &Sender<Msg>,
) {
    let mut reader = BufReader::with_capacity(64 * 1024, stdout);
    let reason = loop {
        match framing::read_message(&mut reader, max_message_bytes) {
            Ok(Some(body)) => match serde_json::from_slice::<Value>(&body) {
                Ok(message) => {
                    let event = ConnEvent::Incoming {
                        generation,
                        message,
                        bytes: body.len(),
                    };
                    if control.send(Msg::Conn(event)).is_err() {
                        return;
                    }
                }
                Err(e) => break format!("server sent malformed JSON: {e}"),
            },
            Ok(None) => break "server closed its output".to_string(),
            Err(FramingError::TooLarge { len, max }) => {
                break format!("server sent a {len}-byte message, above the {max}-byte limit");
            }
            Err(e) => break format!("framing error from server: {e}"),
        }
    };
    let _ = control.send(Msg::Conn(ConnEvent::Lost { generation, reason }));
}

fn writer_loop(
    stdin: ChildStdin,
    generation: u64,
    outgoing: &Receiver<Vec<u8>>,
    control: &Sender<Msg>,
) {
    let mut writer = BufWriter::with_capacity(64 * 1024, stdin);
    for body in outgoing {
        if let Err(e) = framing::write_message(&mut writer, &body) {
            let reason = format!("writing to the server failed: {e}");
            let _ = control.send(Msg::Conn(ConnEvent::Lost { generation, reason }));
            return;
        }
    }
    // Every sender is gone: dropping `writer` closes the server's stdin.
}

fn stderr_loop(mut stderr: ChildStderr, tail: &Mutex<StderrTail>) {
    let mut buf = [0u8; 4096];
    loop {
        match stderr.read(&mut buf) {
            Ok(0) => return,
            Ok(n) => lock(tail).push(&buf[..n]),
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(_) => return,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stderr_tail_keeps_the_most_recent_bytes() {
        let mut tail = StderrTail::new(8);
        tail.push(b"abc");
        assert_eq!(tail.text(), "abc");
        tail.push(b"defgh");
        assert_eq!(tail.text(), "abcdefgh");
        tail.push(b"ij");
        assert_eq!(tail.text(), "cdefghij");
        tail.push(b"0123456789");
        assert_eq!(tail.text(), "23456789");
        let mut none = StderrTail::new(0);
        none.push(b"xyz");
        assert_eq!(none.text(), "");
    }
}
