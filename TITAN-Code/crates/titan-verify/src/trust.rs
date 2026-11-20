//! Trust policy and the inspection that decides whether a check may run.
//!
//! Compiling a Rust workspace runs code from the repository and its dependencies before any
//! test is executed: build scripts (`build.rs`, `package.build`) and procedural macros.
//! Cargo configuration files (`.cargo/config.toml`) can additionally make cargo run other
//! programs (`build.rustc-wrapper`, `target.*.linker`, `target.*.runner`, aliases that
//! shadow `cargo fmt`), and a `rust-toolchain(.toml)` file selects which toolchain binaries
//! rustup runs. The inspection finds all of these; [`TrustInspection::blocking_reason`]
//! turns them into a `NotRun` reason unless the [`TrustPolicy`] allows them.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::check::Check;

/// Most directory entries visited when looking for manifests.
const MAX_WALK_ENTRIES: usize = 200_000;
/// Largest manifest read.
const MAX_MANIFEST_BYTES: u64 = 4 * 1024 * 1024;

/// What project code the pipeline may execute. Everything is denied by default.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct TrustPolicy {
    /// Allow compiling packages (workspace or dependencies) that have build scripts.
    pub allow_build_scripts: bool,
    /// Allow compiling packages that depend on (or are) procedural-macro crates.
    pub allow_proc_macros: bool,
    /// Allow cargo to use the network (otherwise `--offline` / `CARGO_NET_OFFLINE=true`).
    pub allow_network: bool,
}

/// What the inspection of one worktree found. All paths are relative to the worktree
/// unless they lie outside it (ancestors), in which case they are absolute.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct TrustInspection {
    /// Every `Cargo.toml` found in the worktree.
    pub manifests: Vec<String>,
    /// Packages that run a build script, with how it was detected.
    pub build_scripts: Vec<String>,
    /// Procedural-macro packages, with how they were detected.
    pub proc_macros: Vec<String>,
    /// Cargo configuration files cargo would read for this worktree (worktree root and its
    /// ancestors; `$CARGO_HOME/config.toml` is the user's own and not listed).
    pub execution_config: Vec<String>,
    /// `rust-toolchain` / `rust-toolchain.toml` files rustup would read.
    pub toolchain_files: Vec<String>,
    /// Path packages whose manifest resolves outside the worktree.
    pub external_path_packages: Vec<String>,
    /// Number of packages in the resolved dependency set (`cargo metadata`), when it was
    /// determined.
    pub dependency_packages: Option<usize>,
    /// Why the inspection is incomplete (unparsable manifest, `cargo metadata` failure,
    /// walk limit). A non-empty list blocks compiling checks.
    pub problems: Vec<String>,
}

impl TrustInspection {
    /// The `NotRun` reason for `check` under `policy`, or `None` when it may run.
    /// `toolchain_pinned` is true when `RUSTUP_TOOLCHAIN` is passed to the check.
    #[must_use]
    pub fn blocking_reason(
        &self,
        check: &Check,
        policy: &TrustPolicy,
        toolchain_pinned: bool,
    ) -> Option<String> {
        let mut reasons = Vec::new();
        if !self.execution_config.is_empty() {
            reasons.push(format!(
                "cargo configuration files can make cargo run other programs (rustc wrapper, \
                 linker, runner, aliases) and are not allowed: {}",
                self.execution_config.join(", ")
            ));
        }
        if !self.toolchain_files.is_empty() && !toolchain_pinned {
            reasons.push(format!(
                "toolchain files select which toolchain binaries run and RUSTUP_TOOLCHAIN is \
                 not pinned: {}",
                self.toolchain_files.join(", ")
            ));
        }
        if check.compiles_code() {
            if !self.problems.is_empty() {
                reasons.push(format!(
                    "the code this check would compile could not be fully inspected: {}",
                    self.problems.join("; ")
                ));
            }
            if !self.build_scripts.is_empty() && !policy.allow_build_scripts {
                reasons.push(format!(
                    "compiling requires build scripts and the trust policy does not allow \
                     build scripts: {}",
                    self.build_scripts.join(", ")
                ));
            }
            if !self.proc_macros.is_empty() && !policy.allow_proc_macros {
                reasons.push(format!(
                    "compiling requires proc macros and the trust policy does not allow proc \
                     macros: {}",
                    self.proc_macros.join(", ")
                ));
            }
            if !self.external_path_packages.is_empty() {
                reasons.push(format!(
                    "path dependencies outside the worktree are not part of the candidate: {}",
                    self.external_path_packages.join(", ")
                ));
            }
        }
        (!reasons.is_empty()).then(|| reasons.join("; "))
    }
}

/// Build-relevant facts of one manifest.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub(crate) struct ManifestFacts {
    pub package: Option<String>,
    pub build_script: Option<String>,
    pub proc_macro: bool,
}

/// Parses one `Cargo.toml`. `has_build_rs` says whether `build.rs` exists next to it.
pub(crate) fn manifest_facts(text: &str, has_build_rs: bool) -> Result<ManifestFacts, String> {
    let table = text.parse::<toml::Table>().map_err(|e| e.to_string())?;
    let Some(package) = table.get("package").and_then(toml::Value::as_table) else {
        // A virtual manifest builds nothing itself.
        return Ok(ManifestFacts::default());
    };
    let name = package
        .get("name")
        .and_then(toml::Value::as_str)
        .unwrap_or("<unnamed>")
        .to_string();
    let build_script = match package.get("build") {
        Some(toml::Value::Boolean(false)) => None,
        Some(toml::Value::Boolean(true)) => Some("build = true".to_string()),
        Some(toml::Value::String(path)) => Some(format!("build = {path:?}")),
        Some(other) => Some(format!("build = {other}")),
        None => has_build_rs.then(|| "build.rs".to_string()),
    };
    let proc_macro = table
        .get("lib")
        .and_then(toml::Value::as_table)
        .is_some_and(|lib| {
            ["proc-macro", "proc_macro"]
                .iter()
                .any(|key| lib.get(*key).and_then(toml::Value::as_bool) == Some(true))
        });
    Ok(ManifestFacts {
        package: Some(name),
        build_script,
        proc_macro,
    })
}

/// Facts from every manifest under `root`, keyed by relative manifest path. Symbolic links
/// are not followed; `.git` is skipped.
pub(crate) fn walk_manifests(
    root: &Path,
    problems: &mut Vec<String>,
) -> BTreeMap<String, ManifestFacts> {
    let mut found = BTreeMap::new();
    let mut stack: Vec<PathBuf> = vec![root.to_path_buf()];
    let mut visited = 0usize;
    while let Some(dir) = stack.pop() {
        let Ok(entries) = fs::read_dir(&dir) else {
            problems.push(format!("cannot read directory {}", rel(root, &dir)));
            continue;
        };
        for entry in entries.flatten() {
            visited += 1;
            if visited > MAX_WALK_ENTRIES {
                problems.push(format!(
                    "more than {MAX_WALK_ENTRIES} entries; manifest search stopped"
                ));
                return found;
            }
            let Ok(file_type) = entry.file_type() else {
                continue;
            };
            let path = entry.path();
            if file_type.is_dir() {
                if entry.file_name() != ".git" {
                    stack.push(path);
                }
            } else if file_type.is_file() && entry.file_name() == "Cargo.toml" {
                let rel_path = rel(root, &path);
                match read_manifest(&path) {
                    Ok(text) => {
                        let has_build_rs = fs::symlink_metadata(dir.join("build.rs")).is_ok();
                        match manifest_facts(&text, has_build_rs) {
                            Ok(facts) => {
                                found.insert(rel_path, facts);
                            }
                            Err(e) => problems.push(format!("{rel_path}: {e}")),
                        }
                    }
                    Err(e) => problems.push(format!("{rel_path}: {e}")),
                }
            }
        }
    }
    found
}

fn read_manifest(path: &Path) -> Result<String, String> {
    let meta = fs::metadata(path).map_err(|e| e.to_string())?;
    if meta.len() > MAX_MANIFEST_BYTES {
        return Err("manifest is too large".to_string());
    }
    fs::read_to_string(path).map_err(|e| e.to_string())
}

fn rel(root: &Path, path: &Path) -> String {
    match path.strip_prefix(root) {
        Ok(p) if p.as_os_str().is_empty() => ".".to_string(),
        Ok(p) => p.to_string_lossy().replace('\\', "/"),
        Err(_) => path.display().to_string(),
    }
}

/// Cargo configuration and toolchain files that apply to a process started in `root`:
/// those in `root` and in every ancestor. `cargo_home` (the user's own) is excluded.
pub(crate) fn find_config_files(
    root: &Path,
    cargo_home: Option<&Path>,
) -> (Vec<String>, Vec<String>) {
    let trusted: Vec<PathBuf> = cargo_home
        .map(|home| vec![home.join("config"), home.join("config.toml")])
        .unwrap_or_default();
    let mut config = Vec::new();
    let mut toolchain = Vec::new();
    for dir in root.ancestors() {
        for name in [".cargo/config", ".cargo/config.toml"] {
            let path = dir.join(name);
            if fs::symlink_metadata(&path).is_ok() && !trusted.contains(&path) {
                config.push(rel(root, &path));
            }
        }
        for name in ["rust-toolchain", "rust-toolchain.toml"] {
            let path = dir.join(name);
            if fs::symlink_metadata(&path).is_ok() {
                toolchain.push(rel(root, &path));
            }
        }
    }
    (config, toolchain)
}

#[derive(Debug, Deserialize)]
struct Metadata {
    packages: Vec<MetaPackage>,
}

#[derive(Debug, Deserialize)]
struct MetaPackage {
    name: String,
    version: String,
    source: Option<String>,
    manifest_path: PathBuf,
    #[serde(default)]
    targets: Vec<MetaTarget>,
}

#[derive(Debug, Deserialize)]
struct MetaTarget {
    name: String,
    #[serde(default)]
    kind: Vec<String>,
}

/// Fills `inspection` from `cargo metadata --format-version 1` output and the manifest
/// facts. `root` must be canonical.
pub(crate) fn apply_metadata(
    inspection: &mut TrustInspection,
    json: &[u8],
    root: &Path,
    manifests: &BTreeMap<String, ManifestFacts>,
) {
    let metadata: Metadata = match serde_json::from_slice(json) {
        Ok(m) => m,
        Err(e) => {
            inspection
                .problems
                .push(format!("cannot parse cargo metadata output: {e}"));
            return;
        }
    };
    inspection.dependency_packages = Some(metadata.packages.len());
    let mut build = BTreeSet::new();
    let mut procs = BTreeSet::new();
    for package in &metadata.packages {
        let label = format!("`{}` {}", package.name, package.version);
        let local = match fs::canonicalize(&package.manifest_path) {
            Ok(real) => real
                .strip_prefix(root)
                .ok()
                .map(|p| p.to_string_lossy().replace('\\', "/")),
            Err(_) => None,
        };
        match (&local, &package.source) {
            (None, None) => {
                inspection
                    .external_path_packages
                    .push(format!("{label} ({})", package.manifest_path.display()));
            }
            (Some(manifest), _) => {
                // Workspace member or local path dependency: the manifest itself says too.
                if let Some(facts) = manifests.get(manifest) {
                    if let Some(how) = &facts.build_script {
                        build.insert(format!("{label} ({manifest}: {how})"));
                    }
                    if facts.proc_macro {
                        procs.insert(format!("{label} ({manifest}: [lib] proc-macro = true)"));
                    }
                }
            }
            (None, Some(_)) => {}
        }
        let origin = local.clone().unwrap_or_else(|| {
            package
                .source
                .clone()
                .unwrap_or_else(|| package.manifest_path.display().to_string())
        });
        for target in &package.targets {
            if target.kind.iter().any(|k| k == "custom-build") {
                build.insert(format!(
                    "{label} ({origin}: build script target `{}`)",
                    target.name
                ));
            }
            if target.kind.iter().any(|k| k == "proc-macro") {
                procs.insert(format!(
                    "{label} ({origin}: proc-macro target `{}`)",
                    target.name
                ));
            }
        }
    }
    inspection.build_scripts = build.into_iter().collect();
    inspection.proc_macros = procs.into_iter().collect();
}

#[cfg(test)]
mod tests {
    use super::*;

    fn facts(text: &str, build_rs: bool) -> ManifestFacts {
        manifest_facts(text, build_rs).unwrap_or_else(|e| panic!("{e}"))
    }

    #[test]
    fn build_script_detection_follows_cargo_rules() {
        let plain = "[package]\nname = \"a\"\nversion = \"0.1.0\"\n";
        assert_eq!(facts(plain, false).build_script, None);
        assert_eq!(facts(plain, true).build_script.as_deref(), Some("build.rs"));
        let off = format!("{plain}build = false\n");
        assert_eq!(facts(&off, true).build_script, None);
        let custom = format!("{plain}build = \"tools/gen.rs\"\n");
        assert_eq!(
            facts(&custom, false).build_script.as_deref(),
            Some("build = \"tools/gen.rs\"")
        );
        let on = format!("{plain}build = true\n");
        assert!(facts(&on, false).build_script.is_some());
        let virtual_manifest = "[workspace]\nmembers = [\"a\"]\n";
        assert_eq!(facts(virtual_manifest, true), ManifestFacts::default());
        assert!(manifest_facts("[package\nname=", false).is_err());
    }

    #[test]
    fn proc_macro_detection() {
        let base = "[package]\nname = \"m\"\nversion = \"0.1.0\"\n";
        assert!(!facts(base, false).proc_macro);
        assert!(facts(&format!("{base}[lib]\nproc-macro = true\n"), false).proc_macro);
        assert!(facts(&format!("{base}[lib]\nproc_macro = true\n"), false).proc_macro);
        assert!(!facts(&format!("{base}[lib]\nproc-macro = false\n"), false).proc_macro);
    }

    #[test]
    fn blocking_reasons_depend_on_policy_and_check() {
        let inspection = TrustInspection {
            build_scripts: vec!["`a` 0.1.0 (Cargo.toml: build.rs)".into()],
            ..TrustInspection::default()
        };
        let check = Check::CargoCheck { package: None };
        let deny = TrustPolicy::default();
        let reason = inspection.blocking_reason(&check, &deny, false);
        assert!(reason.is_some_and(|r| r.contains("build scripts")));
        assert_eq!(
            inspection.blocking_reason(&Check::CargoFmtCheck, &deny, false),
            None
        );
        let allow = TrustPolicy {
            allow_build_scripts: true,
            ..TrustPolicy::default()
        };
        assert_eq!(inspection.blocking_reason(&check, &allow, false), None);

        let config = TrustInspection {
            execution_config: vec![".cargo/config.toml".into()],
            toolchain_files: vec!["rust-toolchain.toml".into()],
            ..TrustInspection::default()
        };
        let reason = config
            .blocking_reason(&Check::CargoFmtCheck, &allow, false)
            .unwrap_or_default();
        assert!(reason.contains(".cargo/config.toml") && reason.contains("RUSTUP_TOOLCHAIN"));
        let pinned = TrustInspection {
            toolchain_files: vec!["rust-toolchain.toml".into()],
            ..TrustInspection::default()
        };
        assert_eq!(pinned.blocking_reason(&check, &deny, true), None);
    }
}
