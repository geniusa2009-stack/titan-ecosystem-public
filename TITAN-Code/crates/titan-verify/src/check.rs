//! The fixed catalogue of checks and their argument validation.
//!
//! A check is never a command string. Each [`Check`] variant maps to one fixed `cargo`
//! argument vector; the only variable parts are a package name and a test filter, both
//! validated against `[A-Za-z0-9_:-]+` (no leading `-`, bounded length) so they can never
//! become an option, a second argument or shell syntax (no shell is involved anyway).

use std::ffi::OsString;

use serde::{Deserialize, Serialize};

use crate::error::VerifyError;

/// Longest accepted package name or test filter, in bytes.
pub const MAX_ARGUMENT_LEN: usize = 128;

/// One check from the fixed catalogue.
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Check {
    /// `cargo check --all-targets` for the workspace or one package.
    CargoCheck {
        /// `-p <package>`; `None` = `--workspace`.
        package: Option<String>,
    },
    /// `cargo test --no-fail-fast` for the workspace or one package, optionally filtered.
    CargoTest {
        /// `-p <package>`; `None` = `--workspace`.
        package: Option<String>,
        /// Passed to the test harness after `--` (substring filter).
        test_filter: Option<String>,
    },
    /// `cargo build --workspace`.
    CargoBuild,
    /// `cargo fmt --all -- --check` (formats in memory; compiles and runs no project code).
    CargoFmtCheck,
    /// `cargo clippy --workspace --all-targets -- -D warnings`.
    CargoClippy,
}

impl Check {
    /// A short stable name (`cargo-check`, `cargo-test`, ...).
    #[must_use]
    pub fn name(&self) -> &'static str {
        match self {
            Self::CargoCheck { .. } => "cargo-check",
            Self::CargoTest { .. } => "cargo-test",
            Self::CargoBuild => "cargo-build",
            Self::CargoFmtCheck => "cargo-fmt-check",
            Self::CargoClippy => "cargo-clippy",
        }
    }

    /// True when the check compiles project code, which runs build scripts and proc macros
    /// of the workspace and its dependencies (gated by [`crate::TrustPolicy`]).
    #[must_use]
    pub fn compiles_code(&self) -> bool {
        !matches!(self, Self::CargoFmtCheck)
    }

    /// Validates the variable arguments (package name, test filter).
    ///
    /// # Errors
    /// [`VerifyError::InvalidArgument`] when an argument is empty, too long, starts with `-`
    /// or contains a character outside `[A-Za-z0-9_:-]`.
    pub fn validate(&self) -> Result<(), VerifyError> {
        match self {
            Self::CargoCheck { package } => validate_opt("package name", package.as_deref()),
            Self::CargoTest {
                package,
                test_filter,
            } => {
                validate_opt("package name", package.as_deref())?;
                validate_opt("test filter", test_filter.as_deref())
            }
            Self::CargoBuild | Self::CargoFmtCheck | Self::CargoClippy => Ok(()),
        }
    }

    /// The arguments passed to `cargo` (after the program name). `offline` adds `--offline`
    /// to the subcommands that resolve dependencies.
    pub(crate) fn cargo_args(&self, offline: bool) -> Vec<OsString> {
        let mut args: Vec<&str> = Vec::new();
        match self {
            Self::CargoCheck { package } => {
                args.push("check");
                if offline {
                    args.push("--offline");
                }
                push_scope(&mut args, package.as_deref());
                args.push("--all-targets");
            }
            Self::CargoTest {
                package,
                test_filter,
            } => {
                args.push("test");
                if offline {
                    args.push("--offline");
                }
                push_scope(&mut args, package.as_deref());
                args.push("--no-fail-fast");
                if let Some(filter) = test_filter {
                    args.push("--");
                    args.push(filter);
                }
            }
            Self::CargoBuild => {
                args.push("build");
                if offline {
                    args.push("--offline");
                }
                args.push("--workspace");
            }
            // `cargo fmt` is an external subcommand: offline mode comes from the environment.
            Self::CargoFmtCheck => args.extend(["fmt", "--all", "--", "--check"]),
            Self::CargoClippy => {
                args.push("clippy");
                if offline {
                    args.push("--offline");
                }
                args.extend(["--workspace", "--all-targets", "--", "-D", "warnings"]);
            }
        }
        args.into_iter().map(OsString::from).collect()
    }
}

/// `--package <name>` or `--workspace`.
fn push_scope<'a>(args: &mut Vec<&'a str>, package: Option<&'a str>) {
    match package {
        Some(name) => args.extend(["--package", name]),
        None => args.push("--workspace"),
    }
}

fn validate_opt(what: &'static str, value: Option<&str>) -> Result<(), VerifyError> {
    value.map_or(Ok(()), |v| validate_argument(what, v))
}

/// Validates one package name or test filter: 1..=[`MAX_ARGUMENT_LEN`] bytes of
/// `[A-Za-z0-9_:-]`, not starting with `-`.
///
/// # Errors
/// [`VerifyError::InvalidArgument`] with the reason.
pub fn validate_argument(what: &'static str, value: &str) -> Result<(), VerifyError> {
    let reject = |reason: &str| VerifyError::InvalidArgument {
        what,
        value: value.to_string(),
        reason: reason.to_string(),
    };
    if value.is_empty() {
        return Err(reject("must not be empty"));
    }
    if value.len() > MAX_ARGUMENT_LEN {
        return Err(reject("is too long"));
    }
    if value.starts_with('-') {
        return Err(reject("must not start with '-'"));
    }
    if let Some(c) = value
        .chars()
        .find(|c| !(c.is_ascii_alphanumeric() || matches!(c, '_' | ':' | '-')))
    {
        return Err(reject(&format!(
            "contains {c:?}; only [A-Za-z0-9_:-] is allowed"
        )));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn strings(args: Vec<OsString>) -> Vec<String> {
        args.into_iter()
            .map(|a| a.to_string_lossy().into_owned())
            .collect()
    }

    #[test]
    fn argument_vectors_are_fixed() {
        let test = Check::CargoTest {
            package: Some("my-crate".into()),
            test_filter: Some("tests::adds".into()),
        };
        assert_eq!(
            strings(test.cargo_args(true)),
            [
                "test",
                "--offline",
                "--package",
                "my-crate",
                "--no-fail-fast",
                "--",
                "tests::adds"
            ]
        );
        assert_eq!(
            strings(Check::CargoCheck { package: None }.cargo_args(false)),
            ["check", "--workspace", "--all-targets"]
        );
        assert_eq!(
            strings(Check::CargoFmtCheck.cargo_args(true)),
            ["fmt", "--all", "--", "--check"]
        );
        assert!(!Check::CargoFmtCheck.compiles_code());
        assert!(Check::CargoBuild.compiles_code());
    }

    #[test]
    fn shell_metacharacters_and_options_are_rejected() {
        for bad in [
            "a;b",
            "a b",
            "$(id)",
            "`id`",
            "a|b",
            "a&b",
            "a>b",
            "a<b",
            "a'b",
            "a\"b",
            "a\\b",
            "a\nb",
            "a*",
            "a=b",
            "--nocapture",
            "-p",
            "",
            "é",
        ] {
            let check = Check::CargoTest {
                package: None,
                test_filter: Some(bad.to_string()),
            };
            assert!(check.validate().is_err(), "filter {bad:?} was accepted");
            let check = Check::CargoCheck {
                package: Some(bad.to_string()),
            };
            assert!(check.validate().is_err(), "package {bad:?} was accepted");
        }
        let long = "a".repeat(MAX_ARGUMENT_LEN + 1);
        assert!(validate_argument("test filter", &long).is_err());
        for good in ["tests::adds", "my_crate", "my-crate", "a1", "x::y::z_2"] {
            assert!(validate_argument("test filter", good).is_ok(), "{good}");
        }
    }

    #[test]
    fn checks_round_trip_through_json() {
        let checks = vec![
            Check::CargoCheck { package: None },
            Check::CargoTest {
                package: Some("p".into()),
                test_filter: None,
            },
            Check::CargoBuild,
            Check::CargoFmtCheck,
            Check::CargoClippy,
        ];
        let json = serde_json::to_string(&checks).unwrap_or_default();
        assert!(json.contains("\"kind\":\"cargo_fmt_check\""), "{json}");
        let back: Vec<Check> = serde_json::from_str(&json).unwrap_or_default();
        assert_eq!(back, checks);
    }
}
