//! Minimal synchronous client that spawns `titand` and talks to it over stdio.
//! Used by `titan-probe` and the end-to-end tests, so they exercise the real binary.
//!
//! Server notifications and responses to other requests may arrive while waiting for a
//! response; they are buffered, not lost.

use std::collections::{HashMap, VecDeque};
use std::ffi::OsStr;
use std::io::{self, BufReader, Write};
use std::path::Path;
use std::process::{Child, ChildStdin, ChildStdout, Command, ExitStatus, Stdio};

use serde_json::{Value, json};
use titan_protocol::framing::{
    DEFAULT_MAX_MESSAGE_BYTES, FramingError, read_message, write_message,
};
use titan_protocol::rpc::{Request, Response, RpcError};

#[derive(Debug, thiserror::Error)]
pub enum ClientError {
    #[error("I/O error: {0}")]
    Io(#[from] io::Error),
    #[error("framing error: {0}")]
    Framing(#[from] FramingError),
    #[error("invalid JSON from server: {0}")]
    Json(#[from] serde_json::Error),
    #[error("the server closed the connection")]
    Closed,
    #[error("server error {}: {}", .0.code, .0.message)]
    Rpc(RpcError),
}

impl ClientError {
    /// The RPC error code, if the server answered with an error.
    pub fn code(&self) -> Option<i64> {
        match self {
            Self::Rpc(err) => Some(err.code),
            _ => None,
        }
    }
}

pub struct Client {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
    next_id: u64,
    notifications: VecDeque<Value>,
    responses: HashMap<String, Response>,
}

impl Client {
    /// Spawn the server binary at `server`. Its stderr is inherited for diagnostics.
    pub fn spawn(server: &Path) -> Result<Self, ClientError> {
        Self::spawn_with_args::<&str>(server, &[])
    }

    pub fn spawn_with_args<S: AsRef<OsStr>>(
        server: &Path,
        args: &[S],
    ) -> Result<Self, ClientError> {
        let mut child = Command::new(server)
            .args(args)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()?;
        let stdin = child.stdin.take().ok_or(ClientError::Closed)?;
        let stdout = child.stdout.take().ok_or(ClientError::Closed)?;
        Ok(Self {
            child,
            stdin,
            stdout: BufReader::new(stdout),
            next_id: 1,
            notifications: VecDeque::new(),
            responses: HashMap::new(),
        })
    }

    /// Process id of the server.
    pub fn pid(&self) -> u32 {
        self.child.id()
    }

    /// Send a request without waiting; returns its id.
    pub fn send_request(&mut self, method: &str, params: Value) -> Result<u64, ClientError> {
        let id = self.next_id;
        self.next_id += 1;
        let body = serde_json::to_vec(&Request::new(id, method, params))?;
        write_message(&mut self.stdin, &body)?;
        Ok(id)
    }

    pub fn send_notification(&mut self, method: &str, params: Value) -> Result<(), ClientError> {
        let body =
            serde_json::to_vec(&json!({ "jsonrpc": "2.0", "method": method, "params": params }))?;
        write_message(&mut self.stdin, &body)?;
        Ok(())
    }

    /// Read the next message of any kind from the server.
    pub fn read_any(&mut self) -> Result<Value, ClientError> {
        let body = read_message(&mut self.stdout, DEFAULT_MAX_MESSAGE_BYTES)?
            .ok_or(ClientError::Closed)?;
        Ok(serde_json::from_slice(&body)?)
    }

    /// Next response (to any request), buffering notifications.
    pub fn read_response(&mut self) -> Result<Response, ClientError> {
        if let Some(key) = self.responses.keys().next().cloned() {
            if let Some(response) = self.responses.remove(&key) {
                return Ok(response);
            }
        }
        loop {
            let message = self.read_any()?;
            if message.get("method").is_some() {
                self.notifications.push_back(message);
                continue;
            }
            return Ok(serde_json::from_value(message)?);
        }
    }

    /// Wait for the response to request `id`, buffering everything else.
    pub fn wait_response(&mut self, id: u64) -> Result<Response, ClientError> {
        let key = Value::from(id).to_string();
        if let Some(response) = self.responses.remove(&key) {
            return Ok(response);
        }
        loop {
            let message = self.read_any()?;
            if message.get("method").is_some() {
                self.notifications.push_back(message);
                continue;
            }
            let response: Response = serde_json::from_value(message)?;
            if response.id == id {
                return Ok(response);
            }
            self.responses.insert(response.id.to_string(), response);
        }
    }

    /// Send a request and return the full response envelope.
    pub fn request(&mut self, method: &str, params: Value) -> Result<Response, ClientError> {
        let id = self.send_request(method, params)?;
        self.wait_response(id)
    }

    /// Send a request; return its result or the server's error as `ClientError::Rpc`.
    pub fn call(&mut self, method: &str, params: Value) -> Result<Value, ClientError> {
        into_result(self.request(method, params)?)
    }

    /// Wait for a notification with `method`, returning its params. Earlier buffered
    /// notifications with other methods are kept.
    pub fn wait_notification(&mut self, method: &str) -> Result<Value, ClientError> {
        if let Some(pos) = self
            .notifications
            .iter()
            .position(|n| n["method"] == method)
        {
            if let Some(n) = self.notifications.remove(pos) {
                return Ok(n["params"].clone());
            }
        }
        loop {
            let message = self.read_any()?;
            if message.get("method").is_some() {
                if message["method"] == method {
                    return Ok(message["params"].clone());
                }
                self.notifications.push_back(message);
            } else {
                let response: Response = serde_json::from_value(message)?;
                self.responses.insert(response.id.to_string(), response);
            }
        }
    }

    /// Buffered notifications, oldest first.
    pub fn take_notifications(&mut self) -> Vec<Value> {
        self.notifications.drain(..).collect()
    }

    /// Send a raw, already-encoded body (for malformed-input tests).
    pub fn send_raw_body(&mut self, body: &[u8]) -> Result<(), ClientError> {
        write_message(&mut self.stdin, body)?;
        Ok(())
    }

    /// Write raw bytes with no framing at all.
    pub fn send_raw_bytes(&mut self, bytes: &[u8]) -> Result<(), ClientError> {
        self.stdin.write_all(bytes)?;
        self.stdin.flush()?;
        Ok(())
    }

    /// Ask the server to shut down and wait for the process to exit.
    pub fn shutdown(&mut self) -> Result<ExitStatus, ClientError> {
        self.call("session.shutdown", Value::Null)?;
        Ok(self.child.wait()?)
    }

    /// Kill the server process immediately (SIGKILL on Unix), as a crash would.
    pub fn kill(&mut self) -> Result<ExitStatus, ClientError> {
        self.child.kill()?;
        Ok(self.child.wait()?)
    }

    /// Wait for the process to exit (after the server closed the connection).
    pub fn wait(&mut self) -> Result<ExitStatus, ClientError> {
        Ok(self.child.wait()?)
    }
}

/// A response's result, or its error as `ClientError::Rpc`.
pub fn into_result(response: Response) -> Result<Value, ClientError> {
    match response.error {
        Some(err) => Err(ClientError::Rpc(err)),
        None => Ok(response.result.unwrap_or(Value::Null)),
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        // Best effort: the process may already have exited.
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}
