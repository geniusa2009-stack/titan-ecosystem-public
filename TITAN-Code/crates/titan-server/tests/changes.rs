//! Verified Changes end to end: a real rust-analyzer rename becomes a proposal; a concurrent
//! modification makes it stale and it is rejected; a fresh proposal is verified with
//! `cargo test` in isolated worktrees, approved, applied (open buffer + disk file), checked
//! with semantic queries, recorded as evidence, and restored.

use std::fs;
use std::path::Path;
use std::process::Command;
use std::time::{Duration, Instant};

use serde_json::{Value, json};
use titan_protocol::rpc::codes;
use titan_server::client::Client;

fn titand() -> &'static Path {
    Path::new(env!("CARGO_BIN_EXE_titand"))
}

fn rust_analyzer() -> String {
    std::env::var("TITAN_RUST_ANALYZER").expect(
        "set TITAN_RUST_ANALYZER to a rust-analyzer binary (CI: rustup which rust-analyzer)",
    )
}

const LIB: &str = "mod other;\n\npub fn entry() -> u32 {\n    other::helper(41)\n}\n\n#[cfg(test)]\nmod tests {\n    #[test]\n    fn entry_works() {\n        assert_eq!(super::entry(), 42);\n    }\n}\n";
const OTHER: &str = "/// Adds one.\npub fn helper(x: u32) -> u32 {\n    x + 1\n}\n";

fn git(dir: &Path, args: &[&str]) {
    let status = Command::new("git")
        .args(args)
        .current_dir(dir)
        .status()
        .expect("git");
    assert!(status.success(), "git {args:?}");
}

fn fixture() -> tempfile::TempDir {
    let dir = tempfile::tempdir().unwrap();
    let root = dir.path();
    fs::create_dir_all(root.join("src")).unwrap();
    fs::write(
        root.join("Cargo.toml"),
        "[package]\nname = \"fixture\"\nversion = \"0.1.0\"\nedition = \"2021\"\n\n[dependencies]\n",
    )
    .unwrap();
    fs::write(root.join("src/lib.rs"), LIB).unwrap();
    fs::write(root.join("src/other.rs"), OTHER).unwrap();
    fs::write(root.join(".gitignore"), "target/\n").unwrap();
    git(root, &["init", "-q"]);
    git(root, &["config", "user.name", "TITAN test"]);
    git(
        root,
        &["config", "user.email", "titan-test@example.invalid"],
    );
    git(root, &["add", "-A"]);
    git(root, &["commit", "-q", "-m", "fixture"]);
    dir
}

fn wait_ready(client: &mut Client) {
    let deadline = Instant::now() + Duration::from_secs(180);
    loop {
        let s = client.call("intel.status", Value::Null).unwrap()["semantic"].clone();
        if s["state"] == "ready" {
            return;
        }
        assert!(Instant::now() < deadline, "rust-analyzer not ready: {s}");
        std::thread::sleep(Duration::from_millis(100));
    }
}

/// rust-analyzer can need a moment after "ready" before rename answers; retry briefly.
fn rename(client: &mut Client, id: u64, rev: u64) -> Value {
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let r = client.call(
            "refactor.rename",
            json!({ "documentId": id, "expectedRevision": rev,
                    "position": { "line": 3, "character": 12 }, "newName": "add_one" }),
        );
        match r {
            Ok(v)
                if v["proposal"]["changes"]
                    .as_array()
                    .is_some_and(|c| c.len() == 2) =>
            {
                return v;
            }
            other => {
                assert!(
                    Instant::now() < deadline,
                    "rename never produced two changes: {other:?}"
                );
                std::thread::sleep(Duration::from_millis(250));
            }
        }
    }
}

#[test]
#[ignore = "needs rust-analyzer: set TITAN_RUST_ANALYZER (CI does)"]
fn rename_through_verified_changes_with_stale_rejection_verification_and_restore() {
    let project = fixture();
    let root = project.path().canonicalize().unwrap();
    let state = tempfile::tempdir().unwrap();
    let state_arg = state.path().display().to_string();
    let ra = rust_analyzer();
    let mut client = Client::spawn_with_args(
        titand(),
        &[
            "--state-dir",
            state_arg.as_str(),
            "--no-watch",
            "--rust-analyzer",
            ra.as_str(),
        ],
    )
    .unwrap();
    client
        .call("session.hello", json!({ "protocolVersion": 1 }))
        .unwrap();
    client
        .call(
            "workspace.open",
            json!({ "path": root.display().to_string() }),
        )
        .unwrap();
    let id = client
        .call("document.open", json!({ "path": "src/lib.rs" }))
        .unwrap()["documentId"]
        .as_u64()
        .unwrap();
    wait_ready(&mut client);

    // 1-5. Rename request → rust-analyzer WorkspaceEdit → proposal with preview.
    let first = rename(&mut client, id, 0);
    eprintln!("rename proposal: {first:#}");
    assert_eq!(first["proposal"]["source"]["kind"], "lsp.rename");
    assert_eq!(first["status"], "validated");
    let previews = first["validation"]["preview"].as_array().unwrap();
    let diff_of = |path: &str| -> String {
        previews
            .iter()
            .find(|p| p["path"] == path)
            .and_then(|p| p["unifiedDiff"].as_str())
            .unwrap_or_default()
            .to_string()
    };
    assert!(
        diff_of("src/other.rs").contains("-pub fn helper(x: u32) -> u32 {"),
        "{}",
        diff_of("src/other.rs")
    );
    assert!(diff_of("src/other.rs").contains("+pub fn add_one(x: u32) -> u32 {"));
    assert!(diff_of("src/lib.rs").contains("+    other::add_one(41)"));
    let targets: Vec<&str> = previews
        .iter()
        .map(|p| p["target"].as_str().unwrap())
        .collect();
    assert!(
        targets.contains(&"buffer") && targets.contains(&"disk"),
        "{targets:?}"
    );
    let stale_id = first["proposal"]["id"].as_str().unwrap().to_string();

    // 6-7. Someone modifies an affected file; the stale proposal is rejected, nothing written.
    let modified = format!("{OTHER}// edited elsewhere\n");
    fs::write(root.join("src/other.rs"), &modified).unwrap();
    client
        .call(
            "changes.approve",
            json!({ "proposalId": stale_id, "by": "e2e" }),
        )
        .unwrap();
    let err = client
        .call("changes.apply", json!({ "proposalId": stale_id }))
        .unwrap_err();
    assert_eq!(err.code(), Some(codes::STALE_REVISION));
    assert_eq!(
        fs::read_to_string(root.join("src/other.rs")).unwrap(),
        modified
    );
    let text = client
        .call("document.read", json!({ "documentId": id }))
        .unwrap();
    assert_eq!(text["text"], LIB, "the buffer is untouched");
    let rejected = client
        .call("changes.get", json!({ "proposalId": stale_id }))
        .unwrap();
    assert_eq!(rejected["status"], "rejected");

    // 8. A fresh proposal against the current state.
    let fresh = rename(&mut client, id, 0);
    let fresh_id = fresh["proposal"]["id"].as_str().unwrap().to_string();
    assert_ne!(fresh_id, stale_id);
    let preview = client
        .call("changes.preview", json!({ "proposalId": fresh_id }))
        .unwrap();
    assert_eq!(preview["validation"]["ok"], true, "{preview}");

    // 12 (before apply). Verify the candidate: baseline = git HEAD, candidate = HEAD + the
    // proposal, `cargo test` in isolated worktrees; build scripts/proc macros not trusted.
    let verification = client
        .call(
            "changes.verify",
            json!({ "proposalId": fresh_id, "checks": [{ "kind": "cargo_test", "package": null, "test_filter": null }],
                    "timeoutMs": 300000 }),
        )
        .unwrap();
    eprintln!("verification: {verification:#}");
    let runs = verification["report"]["runs"].as_array().unwrap();
    let candidate = runs.iter().find(|r| r["side"] == "candidate").unwrap();
    assert_eq!(candidate["status"]["kind"], "passed", "{candidate}");
    assert!(candidate["tests"]["passed"].as_u64().unwrap() >= 1);
    let checks = verification["evidence"]["checks"].as_array().unwrap();
    assert!(
        checks.iter().any(|c| c["status"]["state"] == "passed"),
        "{checks:?}"
    );

    // 9-10. Explicit approval, then revision-checked application.
    let err = client
        .call("changes.apply", json!({ "proposalId": fresh_id }))
        .unwrap_err();
    assert_eq!(
        err.code(),
        Some(codes::INVALID_PARAMS),
        "apply without approval is refused"
    );
    client
        .call(
            "changes.approve",
            json!({ "proposalId": fresh_id, "by": "e2e" }),
        )
        .unwrap();
    let applied = client
        .call("changes.apply", json!({ "proposalId": fresh_id }))
        .unwrap();
    eprintln!("apply: {applied:#}");
    assert_eq!(applied["outcome"], "applied");
    let on_disk = fs::read_to_string(root.join("src/other.rs")).unwrap();
    assert!(on_disk.contains("pub fn add_one(x: u32)") && on_disk.contains("// edited elsewhere"));
    let buffer = client
        .call("document.read", json!({ "documentId": id }))
        .unwrap();
    assert!(
        buffer["text"]
            .as_str()
            .unwrap()
            .contains("other::add_one(41)")
    );
    assert_eq!(
        buffer["dirty"], true,
        "buffer edits are applied in memory, not saved"
    );

    // 11. Definitions and references after the change (rust-analyzer).
    let rev = buffer["revision"].as_u64().unwrap();
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let d = client
            .call(
                "intel.definition",
                json!({ "documentId": id, "expectedRevision": rev,
                "position": { "line": 3, "character": 12 } }),
            )
            .unwrap();
        let sem = d["results"]
            .as_array()
            .unwrap()
            .iter()
            .find(|e| e["kind"] == "semantic")
            .unwrap()
            .clone();
        if sem["items"][0]["path"] == "src/other.rs" {
            assert_eq!(sem["status"], "current");
            break;
        }
        assert!(
            Instant::now() < deadline,
            "no definition after the rename: {d}"
        );
        std::thread::sleep(Duration::from_millis(250));
    }

    // 13. Evidence on disk.
    let evidence_path = applied["evidencePath"].as_str().unwrap();
    let evidence: Value =
        serde_json::from_str(&fs::read_to_string(evidence_path).unwrap()).unwrap();
    assert_eq!(evidence["outcome"], "applied");
    assert_eq!(evidence["approval"]["by"], "e2e");
    assert!(
        evidence["checks"]
            .as_array()
            .unwrap()
            .iter()
            .any(|c| c["name"] == "cargo_test")
    );
    assert_eq!(evidence["affected"].as_array().unwrap().len(), 2);
    assert_eq!(evidence["restore"]["available"], true);

    // 14. Restore: disk and buffer back to what they were before the apply.
    let restored = client
        .call("changes.restore", json!({ "proposalId": fresh_id }))
        .unwrap();
    eprintln!("restore: {restored:#}");
    assert_eq!(restored["restored"], true);
    assert_eq!(
        fs::read_to_string(root.join("src/other.rs")).unwrap(),
        modified
    );
    let buffer = client
        .call("document.read", json!({ "documentId": id }))
        .unwrap();
    assert_eq!(buffer["text"], LIB);
    assert!(client.shutdown().unwrap().success());
}

/// A developer proposal (no language server): preview, unsaved-edit protection, path
/// confinement, no-clobber create, apply, restore.
#[test]
fn developer_proposals_are_validated_applied_and_restored() {
    let project = tempfile::tempdir().unwrap();
    let root = project.path().canonicalize().unwrap();
    fs::write(root.join("a.txt"), "alpha\n").unwrap();
    fs::write(root.join("b.txt"), "beta\n").unwrap();
    let state = tempfile::tempdir().unwrap();
    let state_arg = state.path().display().to_string();
    let mut client =
        Client::spawn_with_args(titand(), &["--state-dir", state_arg.as_str(), "--no-watch"])
            .unwrap();
    client
        .call("session.hello", json!({ "protocolVersion": 1 }))
        .unwrap();
    client
        .call(
            "workspace.open",
            json!({ "path": root.display().to_string() }),
        )
        .unwrap();
    let hash = |text: &str| titan_doc::ContentHash::of(text.as_bytes()).to_hex();

    // Escaping paths and clobbering creates are problems; nothing is written.
    let bad = client
        .call(
            "changes.propose",
            json!({ "changes": [
                { "kind": "create", "path": "../escape.txt", "content": "x" },
                { "kind": "create", "path": "b.txt", "content": "clobber" },
            ]}),
        )
        .unwrap();
    assert_eq!(bad["status"], "rejected");
    let kinds: Vec<&str> = bad["validation"]["problems"]
        .as_array()
        .unwrap()
        .iter()
        .map(|p| p["kind"].as_str().unwrap())
        .collect();
    assert!(kinds.len() >= 2, "{kinds:?}");
    assert!(!root.parent().unwrap().join("escape.txt").exists());

    // An open document with unsaved edits is protected: a disk expectation is refused.
    let doc = client
        .call("document.open", json!({ "path": "a.txt" }))
        .unwrap()["documentId"]
        .as_u64()
        .unwrap();
    client
        .call(
            "document.applyEdits",
            json!({ "documentId": doc, "expectedRevision": 0, "edits": [
                { "range": { "start": { "line": 0, "character": 0 }, "end": { "line": 0, "character": 0 } },
                  "text": "unsaved " } ] }),
        )
        .unwrap();
    let protected = client
        .call(
            "changes.propose",
            json!({ "changes": [{ "kind": "edit", "path": "a.txt",
                "expected": { "kind": "disk", "hash": hash("alpha\n"), "len": 6 },
                "edits": [{ "start": 0, "end": 5, "text": "ALPHA" }] }]}),
        )
        .unwrap();
    assert_eq!(protected["status"], "rejected", "{protected}");

    // A valid multi-file proposal: buffer edit + disk edit + new file.
    let good = client
        .call(
            "changes.propose",
            json!({ "changes": [
                { "kind": "edit", "path": "a.txt",
                  "expected": { "kind": "buffer", "revision": 1, "hash": hash("unsaved alpha\n") },
                  "edits": [{ "start": 8, "end": 13, "text": "ALPHA" }] },
                { "kind": "edit", "path": "b.txt",
                  "expected": { "kind": "disk", "hash": hash("beta\n"), "len": 5 },
                  "edits": [{ "start": 0, "end": 4, "text": "BETA" }] },
                { "kind": "create", "path": "c.txt", "content": "gamma\n" },
            ]}),
        )
        .unwrap();
    assert_eq!(good["status"], "validated", "{good}");
    let pid = good["proposal"]["id"].as_str().unwrap().to_string();
    client
        .call("changes.approve", json!({ "proposalId": pid }))
        .unwrap();
    let applied = client
        .call("changes.apply", json!({ "proposalId": pid }))
        .unwrap();
    assert_eq!(applied["outcome"], "applied", "{applied}");
    assert_eq!(fs::read_to_string(root.join("b.txt")).unwrap(), "BETA\n");
    assert_eq!(fs::read_to_string(root.join("c.txt")).unwrap(), "gamma\n");
    assert_eq!(
        fs::read_to_string(root.join("a.txt")).unwrap(),
        "alpha\n",
        "buffers are not saved"
    );
    let text = client
        .call("document.read", json!({ "documentId": doc }))
        .unwrap();
    assert_eq!(text["text"], "unsaved ALPHA\n");
    let checks = applied["evidence"]["checks"].as_array().unwrap();
    assert_eq!(
        checks[0]["status"]["state"], "notRun",
        "no check ran, and the evidence says so"
    );

    let restored = client
        .call("changes.restore", json!({ "proposalId": pid }))
        .unwrap();
    assert_eq!(restored["restored"], true, "{restored}");
    assert_eq!(fs::read_to_string(root.join("b.txt")).unwrap(), "beta\n");
    assert!(!root.join("c.txt").exists());
    let text = client
        .call("document.read", json!({ "documentId": doc }))
        .unwrap();
    assert_eq!(text["text"], "unsaved alpha\n");
    let list = client.call("changes.list", Value::Null).unwrap();
    assert_eq!(list["proposals"].as_array().unwrap().len(), 3);
    assert!(client.shutdown().unwrap().success());
}
