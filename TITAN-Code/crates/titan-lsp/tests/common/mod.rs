//! Shared helpers for the integration tests: an event recorder and process checks.
#![allow(dead_code)]

use std::process::{Command, Stdio};
use std::sync::{Arc, Condvar, Mutex, PoisonError};
use std::time::{Duration, Instant};

use serde_json::Value;
use titan_lsp::{EventCallback, LspError, LspEvent, ServerState};

/// Records every event; tests wait on predicates over the history.
#[derive(Clone)]
pub struct Events {
    inner: Arc<(Mutex<Vec<LspEvent>>, Condvar)>,
}

impl Events {
    pub fn new() -> (Self, EventCallback) {
        let events = Self {
            inner: Arc::new((Mutex::new(Vec::new()), Condvar::new())),
        };
        let sink = events.clone();
        let callback: EventCallback = Box::new(move |event| {
            let (lock, cvar) = &*sink.inner;
            lock.lock()
                .unwrap_or_else(PoisonError::into_inner)
                .push(event);
            cvar.notify_all();
        });
        (events, callback)
    }

    pub fn snapshot(&self) -> Vec<LspEvent> {
        self.inner
            .0
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .clone()
    }

    /// Number of events recorded so far (a cursor for [`Self::wait_after`]).
    pub fn mark(&self) -> usize {
        self.snapshot().len()
    }

    pub fn count(&self, pred: impl Fn(&LspEvent) -> bool) -> usize {
        self.snapshot().iter().filter(|e| pred(e)).count()
    }

    pub fn wait_for(
        &self,
        timeout: Duration,
        what: &str,
        pred: impl Fn(&LspEvent) -> bool,
    ) -> LspEvent {
        self.wait_after(0, timeout, what, pred)
    }

    /// Waits for the first event at index >= `from` matching `pred`.
    pub fn wait_after(
        &self,
        from: usize,
        timeout: Duration,
        what: &str,
        pred: impl Fn(&LspEvent) -> bool,
    ) -> LspEvent {
        let deadline = Instant::now() + timeout;
        let (lock, cvar) = &*self.inner;
        let mut events = lock.lock().unwrap_or_else(PoisonError::into_inner);
        loop {
            if let Some(event) = events.iter().skip(from).find(|e| pred(e)) {
                return event.clone();
            }
            let now = Instant::now();
            if now >= deadline {
                let dump: Vec<String> = events.iter().map(short).collect();
                panic!(
                    "timed out after {timeout:?} waiting for {what}; events so far:\n{}",
                    dump.join("\n")
                );
            }
            events = cvar
                .wait_timeout(events, deadline - now)
                .unwrap_or_else(PoisonError::into_inner)
                .0;
        }
    }

    /// Waits for the completion of `ticket`; returns (generation, result).
    pub fn response(&self, ticket: u64, timeout: Duration) -> (u64, Result<Value, LspError>) {
        match self.wait_for(
            timeout,
            &format!("response to ticket {ticket}"),
            |e| matches!(e, LspEvent::Response { ticket: t, .. } if *t == ticket),
        ) {
            LspEvent::Response {
                generation, result, ..
            } => (generation, result),
            other => panic!("unexpected event {other:?}"),
        }
    }

    pub fn responses_for(&self, ticket: u64) -> usize {
        self.count(|e| matches!(e, LspEvent::Response { ticket: t, .. } if *t == ticket))
    }

    pub fn wait_state(
        &self,
        timeout: Duration,
        what: &str,
        pred: impl Fn(u64, &ServerState) -> bool,
    ) -> (u64, ServerState) {
        match self.wait_for(
            timeout,
            what,
            |e| matches!(e, LspEvent::State { generation, state } if pred(*generation, state)),
        ) {
            LspEvent::State { generation, state } => (generation, state),
            other => panic!("unexpected event {other:?}"),
        }
    }

    pub fn wait_running(&self, generation: u64, timeout: Duration) {
        self.wait_state(
            timeout,
            &format!("generation {generation} running"),
            |g, s| g == generation && s.is_running(),
        );
    }

    pub fn wait_notification(
        &self,
        from: usize,
        timeout: Duration,
        method: &str,
        pred: impl Fn(u64, &Value) -> bool,
    ) -> (u64, Value) {
        match self.wait_after(from, timeout, &format!("notification {method}"), |e| {
            matches!(e, LspEvent::Notification { generation, method: m, params }
                if m == method && pred(*generation, params))
        }) {
            LspEvent::Notification {
                generation, params, ..
            } => (generation, params),
            other => panic!("unexpected event {other:?}"),
        }
    }
}

fn short(event: &LspEvent) -> String {
    let mut text = format!("{event:?}");
    if text.len() > 400 {
        let mut cut = 400;
        while !text.is_char_boundary(cut) {
            cut -= 1;
        }
        text.truncate(cut);
        text.push_str("...");
    }
    text
}

/// True if a process with this pid exists (zombies included, so a reaped child is gone).
pub fn process_alive(pid: u32) -> bool {
    Command::new("kill")
        .args(["-0", &pid.to_string()])
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|status| status.success())
        .unwrap_or(false)
}

pub fn kill_9(pid: u32) {
    let status = Command::new("kill")
        .args(["-9", &pid.to_string()])
        .status()
        .expect("run kill");
    assert!(status.success(), "kill -9 {pid} failed");
}
