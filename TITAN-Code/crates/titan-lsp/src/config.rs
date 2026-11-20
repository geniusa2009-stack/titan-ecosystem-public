//! Supervisor configuration: what to run and the limits it runs under.

use std::path::PathBuf;
use std::time::Duration;

use serde_json::Value;

/// How to start a language server: a program and its arguments, never a shell command line.
#[derive(Clone, Debug, PartialEq)]
pub struct LspCommand {
    pub program: PathBuf,
    pub args: Vec<String>,
    /// Working directory of the server process.
    pub cwd: PathBuf,
    /// Extra environment variables (the server otherwise inherits this process's environment).
    pub env: Vec<(String, String)>,
}

impl LspCommand {
    /// A command with no arguments, run in the current directory (`"."`).
    pub fn new(program: impl Into<PathBuf>) -> Self {
        Self {
            program: program.into(),
            args: Vec::new(),
            cwd: PathBuf::from("."),
            env: Vec::new(),
        }
    }

    pub fn arg(mut self, arg: impl Into<String>) -> Self {
        self.args.push(arg.into());
        self
    }

    pub fn args<I, S>(mut self, args: I) -> Self
    where
        I: IntoIterator<Item = S>,
        S: Into<String>,
    {
        self.args.extend(args.into_iter().map(Into::into));
        self
    }

    pub fn cwd(mut self, cwd: impl Into<PathBuf>) -> Self {
        self.cwd = cwd.into();
        self
    }

    pub fn env(mut self, key: impl Into<String>, value: impl Into<String>) -> Self {
        self.env.push((key.into(), value.into()));
        self
    }
}

/// Everything the supervisor needs. Build it with [`SupervisorConfig::new`] and override the
/// fields that matter; the defaults are documented on each field.
#[derive(Clone, Debug, PartialEq)]
pub struct SupervisorConfig {
    pub command: LspCommand,
    /// Workspace root, sent as `rootUri` and as the single entry of `workspaceFolders`.
    /// Should be absolute; a relative path is resolved against the current directory.
    pub root: PathBuf,
    /// Sent as `initializationOptions` (omitted when `null`). Default `null`.
    pub initialization_options: Value,
    /// Answers `workspace/configuration`: for each requested item, the value at its
    /// `section` (looked up as a key first, then as a dotted path), or `null` if absent.
    /// An item without a section receives the whole value. Default `{}`.
    pub workspace_configuration: Value,
    /// Deadline for the `initialize` response. Default 30 s.
    pub init_timeout: Duration,
    /// Deadline for requests that do not pass their own. Default 30 s.
    pub request_timeout: Duration,
    /// Time [`crate::Supervisor::shutdown`] waits for `shutdown` + `exit` before killing.
    /// Default 5 s.
    pub shutdown_timeout: Duration,
    /// Largest accepted message body; a larger one is a protocol failure that ends the
    /// connection. Default 64 MiB.
    pub max_message_bytes: usize,
    /// Restarts allowed within `restart_window`; one crash more and the supervisor gives up
    /// ([`crate::ServerState::Failed`]). Default 3.
    pub max_restarts: u32,
    /// Sliding window over which crashes are counted. Default 60 s.
    pub restart_window: Duration,
    /// Delay before the first restart; doubled for each further crash in the window.
    /// Default 500 ms.
    pub backoff_initial: Duration,
    /// Upper bound of the restart delay. Default 30 s.
    pub backoff_max: Duration,
    /// Capacity (in messages) of the outgoing queue of one connection. When it is full,
    /// `request`/`notify` fail with [`crate::LspError::QueueFull`] instead of blocking.
    /// Default 4096.
    pub outgoing_queue: usize,
    /// Bytes of server stderr kept for crash reports (the most recent ones). Default 64 KiB.
    pub stderr_tail_bytes: usize,
}

impl SupervisorConfig {
    pub fn new(command: LspCommand, root: impl Into<PathBuf>) -> Self {
        Self {
            command,
            root: root.into(),
            initialization_options: Value::Null,
            workspace_configuration: Value::Object(serde_json::Map::new()),
            init_timeout: Duration::from_secs(30),
            request_timeout: Duration::from_secs(30),
            shutdown_timeout: Duration::from_secs(5),
            max_message_bytes: titan_protocol::framing::DEFAULT_MAX_MESSAGE_BYTES,
            max_restarts: 3,
            restart_window: Duration::from_secs(60),
            backoff_initial: Duration::from_millis(500),
            backoff_max: Duration::from_secs(30),
            outgoing_queue: 4096,
            stderr_tail_bytes: 64 * 1024,
        }
    }

    /// Delay before restart number `attempt` (1-based): `backoff_initial * 2^(attempt-1)`,
    /// capped at `backoff_max`.
    pub fn backoff(&self, attempt: u32) -> Duration {
        let exponent = attempt.saturating_sub(1).min(30);
        let factor = 1u32 << exponent;
        self.backoff_initial
            .checked_mul(factor)
            .unwrap_or(self.backoff_max)
            .min(self.backoff_max)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn backoff_doubles_and_caps() {
        let mut config = SupervisorConfig::new(LspCommand::new("x"), "/");
        config.backoff_initial = Duration::from_millis(100);
        config.backoff_max = Duration::from_millis(700);
        assert_eq!(config.backoff(0), Duration::from_millis(100));
        assert_eq!(config.backoff(1), Duration::from_millis(100));
        assert_eq!(config.backoff(2), Duration::from_millis(200));
        assert_eq!(config.backoff(3), Duration::from_millis(400));
        assert_eq!(config.backoff(4), Duration::from_millis(700));
        assert_eq!(config.backoff(u32::MAX), Duration::from_millis(700));
    }

    #[test]
    fn command_builder() {
        let cmd = LspCommand::new("ra")
            .arg("--a")
            .args(["b", "c"])
            .cwd("/w")
            .env("K", "V");
        assert_eq!(cmd.args, vec!["--a", "b", "c"]);
        assert_eq!(cmd.cwd, PathBuf::from("/w"));
        assert_eq!(cmd.env, vec![("K".to_string(), "V".to_string())]);
    }
}
