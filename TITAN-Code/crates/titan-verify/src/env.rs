//! Program resolution and the environment passed to child processes.
//!
//! Children start from an empty environment. Checks get only `PATH`, `HOME`, `RUSTUP_HOME`,
//! `CARGO_HOME`, `RUSTUP_TOOLCHAIN` (each only when set), a per-side `CARGO_TARGET_DIR`,
//! `CARGO_TERM_COLOR=never` and, without network permission, `CARGO_NET_OFFLINE=true`.
//! Git gets `PATH`, `HOME` and `GIT_TERMINAL_PROMPT=0`.

use std::collections::BTreeMap;
use std::ffi::{OsStr, OsString};
use std::path::{Path, PathBuf};

use crate::error::VerifyError;

/// Snapshot of the variables of the parent environment the pipeline may forward.
#[derive(Debug, Clone, Default)]
pub(crate) struct HostEnv {
    vars: BTreeMap<&'static str, OsString>,
}

/// Variables read from the parent environment; nothing else is ever forwarded.
const FORWARDED: [&str; 5] = [
    "PATH",
    "HOME",
    "RUSTUP_HOME",
    "CARGO_HOME",
    "RUSTUP_TOOLCHAIN",
];

impl HostEnv {
    pub(crate) fn capture() -> Self {
        let mut vars = BTreeMap::new();
        for name in FORWARDED {
            if let Some(value) = std::env::var_os(name) {
                vars.insert(name, value);
            }
        }
        Self { vars }
    }

    pub(crate) fn get(&self, name: &str) -> Option<&OsStr> {
        self.vars.get(name).map(OsString::as_os_str)
    }

    pub(crate) fn set(&mut self, name: &'static str, value: OsString) {
        self.vars.insert(name, value);
    }

    /// `$CARGO_HOME`, or `$HOME/.cargo`.
    pub(crate) fn cargo_home(&self) -> Option<PathBuf> {
        self.get("CARGO_HOME")
            .map(PathBuf::from)
            .or_else(|| self.get("HOME").map(|h| Path::new(h).join(".cargo")))
    }

    /// Environment of a cargo check.
    pub(crate) fn for_cargo(&self, target_dir: &Path, offline: bool) -> Vec<(OsString, OsString)> {
        let mut env: Vec<(OsString, OsString)> = self
            .vars
            .iter()
            .map(|(k, v)| (OsString::from(k), v.clone()))
            .collect();
        env.push(("CARGO_TARGET_DIR".into(), target_dir.as_os_str().to_owned()));
        env.push(("CARGO_TERM_COLOR".into(), "never".into()));
        if offline {
            env.push(("CARGO_NET_OFFLINE".into(), "true".into()));
        }
        env
    }

    /// Environment of a git invocation.
    pub(crate) fn for_git(&self) -> Vec<(OsString, OsString)> {
        let mut env: Vec<(OsString, OsString)> = ["PATH", "HOME"]
            .into_iter()
            .filter_map(|k| self.get(k).map(|v| (OsString::from(k), v.to_owned())))
            .collect();
        env.push(("GIT_TERMINAL_PROMPT".into(), "0".into()));
        env
    }

    /// Resolves `name` to an absolute executable: the explicit path when given (it must
    /// exist), otherwise the first match on this snapshot's `PATH`.
    pub(crate) fn resolve(
        &self,
        name: &'static str,
        explicit: Option<&Path>,
    ) -> Result<PathBuf, VerifyError> {
        if let Some(path) = explicit {
            return if path.is_absolute() && is_executable(path) {
                Ok(path.to_path_buf())
            } else {
                Err(VerifyError::ToolNotFound {
                    tool: name,
                    detail: format!(
                        "configured path {} is not an absolute path to an executable file",
                        path.display()
                    ),
                })
            };
        }
        let Some(path_var) = self.get("PATH") else {
            return Err(VerifyError::ToolNotFound {
                tool: name,
                detail: "PATH is not set".to_string(),
            });
        };
        for dir in std::env::split_paths(path_var) {
            if !dir.is_absolute() {
                // A relative PATH entry would resolve against the worktree: never use it.
                continue;
            }
            let candidate = dir.join(name);
            if is_executable(&candidate) {
                return Ok(candidate);
            }
        }
        Err(VerifyError::ToolNotFound {
            tool: name,
            detail: "not on PATH".to_string(),
        })
    }
}

#[cfg(unix)]
fn is_executable(path: &Path) -> bool {
    use std::os::unix::fs::PermissionsExt;
    std::fs::metadata(path).is_ok_and(|m| m.is_file() && m.permissions().mode() & 0o111 != 0)
}

#[cfg(not(unix))]
fn is_executable(path: &Path) -> bool {
    std::fs::metadata(path).is_ok_and(|m| m.is_file())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cargo_environment_is_an_allow_list() {
        let mut host = HostEnv::default();
        host.set("PATH", "/usr/bin".into());
        host.set("HOME", "/home/u".into());
        let env = host.for_cargo(Path::new("/tmp/run/target"), true);
        let names: Vec<String> = env
            .iter()
            .map(|(k, _)| k.to_string_lossy().into_owned())
            .collect();
        assert_eq!(
            names,
            [
                "HOME",
                "PATH",
                "CARGO_TARGET_DIR",
                "CARGO_TERM_COLOR",
                "CARGO_NET_OFFLINE"
            ]
        );
        let online = host.for_cargo(Path::new("/t"), false);
        assert!(!online.iter().any(|(k, _)| k == "CARGO_NET_OFFLINE"));
    }

    #[test]
    fn capture_reads_only_forwarded_names() {
        let host = HostEnv::capture();
        assert!(host.vars.keys().all(|k| FORWARDED.contains(k)));
    }
}
