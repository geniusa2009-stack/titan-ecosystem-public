//! `titan-bench`: reproducible performance laboratory for the TITAN Code core.
//!
//! * Datasets are generated from fixed seeds.
//! * Each scenario records per-operation wall-clock samples (min / median / p95 / p99 /
//!   max, nanoseconds) plus heap allocations and allocated bytes per operation, measured
//!   with an instrumented global allocator (`stats_alloc`, which adds a few atomic
//!   increments to every allocation; latencies include that overhead).
//! * Process memory (`VmRSS`, `VmHWM`) is read from `/proc/self/status` on Linux; it is
//!   reported as `null` elsewhere.
//! * With `--server PATH`, protocol round trips are measured against a real `titand`.
//!
//! Numbers only describe the machine and build they were produced on. Never compare them
//! across machines, and never read a microbenchmark as a complete-editor comparison.
//!
//! * With `--server PATH`, system scenarios ([`load`]) measure editing latency while the
//!   index scans, searches are cancelled in bulk, files change on disk and (with
//!   `--rust-analyzer PATH`) rust-analyzer loads and analyses a Cargo workspace, plus memory
//!   and CPU of `titand` and of rust-analyzer separately. `--no-load` skips them.
//!
//! * [`profile`]: memory per session phase (live heap with `--profile-server`, a `titand`
//!   built with the `alloc-stats` feature) and rust-analyzer cold start vs warm queries.
//!
//! Usage: titan-bench [--quick] [--out PATH] [--server PATH] [--rust-analyzer PATH]
//!                    [--profile-server PATH] [--no-load]

mod load;
mod profile;

use std::alloc::System;
use std::env;
use std::fs;
use std::hint::black_box;
use std::path::Path;
use std::time::Instant;

use serde_json::{Value, json};
use stats_alloc::{INSTRUMENTED_SYSTEM, Region, StatsAlloc};
use titan_doc::{Document, Limits};
use titan_index::{SyntaxParser, input_edits};
use titan_recovery::{BaseInfo, JournalConfig, JournalWriter};
use titan_text::{ByteOffset, TextBuffer, TextDocument, TextEdit, Utf16Offset};
use titan_workspace::{SearchQuery, Workspace};

#[global_allocator]
static GLOBAL: &StatsAlloc<System> = &INSTRUMENTED_SYSTEM;

struct Rng(u64);

impl Rng {
    fn next_u64(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.0 = x;
        x.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }

    fn below(&mut self, n: usize) -> usize {
        if n == 0 {
            0
        } else {
            (self.next_u64() % n as u64) as usize
        }
    }
}

/// Deterministic Rust-like text of roughly `target` bytes (same generator as before, so
/// results stay comparable with the previous harness).
fn code_text(target: usize, seed: u64) -> String {
    let mut rng = Rng(seed);
    let words = [
        "let", "value", "fn", "match", "Some", "None", "self", "impl", "struct", "return",
    ];
    let mut out = String::with_capacity(target + 128);
    while out.len() < target {
        let indent = rng.below(4) * 4;
        out.push_str(&" ".repeat(indent));
        for _ in 0..(3 + rng.below(8)) {
            out.push_str(words[rng.below(words.len())]);
            out.push(' ');
        }
        out.push_str("{}\n");
    }
    out
}

/// Valid Rust source of roughly `target` bytes, for parser benchmarks.
fn rust_source(target: usize) -> String {
    let mut out = String::with_capacity(target + 256);
    let mut i = 0;
    while out.len() < target {
        out.push_str(&format!(
            "/// Item {i}.\npub fn function_{i}(x: u64) -> u64 {{\n    let y = x.wrapping_mul({i});\n    if y > 10 {{ y - 1 }} else {{ y + 1 }}\n}}\n\n"
        ));
        i += 1;
    }
    out
}

fn unicode_text(target: usize, seed: u64) -> String {
    let mut rng = Rng(seed);
    let pieces = [
        "مرحبا ",
        "بالعالم ",
        "hello ",
        "world ",
        "😀 ",
        "👨\u{200d}👩\u{200d}👧 ",
        "é ",
        "日本語 ",
    ];
    let mut out = String::with_capacity(target + 64);
    while out.len() < target {
        for _ in 0..(4 + rng.below(8)) {
            out.push_str(pieces[rng.below(pieces.len())]);
        }
        out.push('\n');
    }
    out
}

fn single_line(target: usize) -> String {
    "abcdefgh".repeat(target / 8)
}

fn boundary(buffer: &TextBuffer, offset: usize) -> ByteOffset {
    let mut at = offset.min(buffer.len_bytes());
    while buffer.check_offset(ByteOffset(at)).is_err() {
        at -= 1;
    }
    ByteOffset(at)
}

#[derive(Default)]
struct Scenario {
    samples: Vec<u128>,
    allocations: usize,
    bytes_allocated: usize,
    ops: usize,
}

impl Scenario {
    /// Time `f` once and count its allocations.
    fn run<T>(&mut self, f: impl FnOnce() -> T) -> T {
        let region = Region::new(GLOBAL);
        let start = Instant::now();
        let out = f();
        let elapsed = start.elapsed().as_nanos();
        let change = region.change();
        self.samples.push(elapsed);
        self.allocations += change.allocations + change.reallocations;
        self.bytes_allocated += change.bytes_allocated;
        self.ops += 1;
        out
    }

    /// Merge the samples of another run of the same operation.
    fn absorb(&mut self, other: Scenario) {
        self.samples.extend(other.samples);
        self.allocations += other.allocations;
        self.bytes_allocated += other.bytes_allocated;
        self.ops += other.ops;
    }

    fn summary(mut self, name: &str, dataset: &str, bytes: usize) -> Value {
        self.samples.sort_unstable();
        let samples = &self.samples;
        let pick = |q: f64| -> u128 {
            if samples.is_empty() {
                return 0;
            }
            samples[((samples.len() - 1) as f64 * q).round() as usize]
        };
        let ops = self.ops.max(1);
        json!({
            "scenario": name,
            "dataset": dataset,
            "datasetBytes": bytes,
            "samples": samples.len(),
            "unit": "ns",
            "min": samples.first().copied().unwrap_or(0),
            "median": pick(0.5),
            "p95": pick(0.95),
            "p99": if samples.len() >= 100 { json!(pick(0.99)) } else { Value::Null },
            "max": samples.last().copied().unwrap_or(0),
            "allocationsPerOp": self.allocations as f64 / ops as f64,
            "bytesAllocatedPerOp": self.bytes_allocated as f64 / ops as f64,
        })
    }
}

fn text_scenarios(name: &str, text: &str, ops: usize, out: &mut Vec<Value>) {
    let bytes = text.len();
    let mut s = Scenario::default();
    for _ in 0..5 {
        drop(black_box(s.run(|| TextDocument::new(black_box(text)))));
    }
    out.push(s.summary("open", name, bytes));

    let mut doc = TextDocument::new(text);
    let mut rng = Rng(0xA11CE);
    let mut s = Scenario::default();
    for _ in 0..ops {
        let at = boundary(doc.buffer(), rng.below(doc.buffer().len_bytes() + 1));
        let rev = doc.revision();
        let _ = s.run(|| doc.apply(rev, vec![TextEdit::insert(at, "x")]));
    }
    out.push(s.summary("random_insert", name, bytes));

    let mut s = Scenario::default();
    for _ in 0..ops {
        let rev = doc.revision();
        let _ = s.run(|| doc.undo(rev));
    }
    out.push(s.summary("undo", name, bytes));
    let mut s = Scenario::default();
    for _ in 0..ops {
        let rev = doc.revision();
        let _ = s.run(|| doc.redo(rev));
    }
    out.push(s.summary("redo", name, bytes));

    let mut s = Scenario::default();
    for _ in 0..ops {
        let len = doc.buffer().len_bytes();
        let start = boundary(doc.buffer(), rng.below(len + 1));
        let end = boundary(doc.buffer(), (start.0 + 1 + rng.below(16)).min(len)).max(start);
        let rev = doc.revision();
        let _ = s.run(|| doc.apply(rev, vec![TextEdit::delete(start, end)]));
    }
    out.push(s.summary("random_delete", name, bytes));

    let mut doc = TextDocument::new(text);
    let mut cursor = boundary(doc.buffer(), bytes / 2);
    let mut s = Scenario::default();
    for _ in 0..ops {
        let rev = doc.revision();
        let _ = s.run(|| doc.apply(rev, vec![TextEdit::insert(cursor, "a")]));
        cursor = ByteOffset(cursor.0 + 1);
    }
    out.push(s.summary("sequential_typing", name, bytes));

    let mut s = Scenario::default();
    for _ in 0..(ops / 10).max(1) {
        let len = doc.buffer().len_bytes();
        let edits: Vec<TextEdit> = (0..10)
            .map(|i| TextEdit::insert(boundary(doc.buffer(), len / 10 * i), "//"))
            .collect();
        let rev = doc.revision();
        let _ = s.run(|| doc.apply(rev, edits));
    }
    out.push(s.summary("multi_cursor_10", name, bytes));

    let mut s = Scenario::default();
    for _ in 0..ops {
        drop(black_box(s.run(|| doc.snapshot())));
    }
    out.push(s.summary("snapshot", name, bytes));

    let buffer = doc.buffer().clone();
    let len = buffer.len_bytes();
    let mut s = Scenario::default();
    for _ in 0..ops {
        let at = boundary(&buffer, rng.below(len + 1));
        black_box(s.run(|| buffer.byte_to_utf16_point(at).ok()));
    }
    out.push(s.summary("byte_to_utf16_point", name, bytes));
    let utf16_len = buffer.len_utf16();
    let mut s = Scenario::default();
    for _ in 0..ops {
        let at = Utf16Offset(rng.below(utf16_len + 1));
        black_box(s.run(|| buffer.utf16_to_byte(at).ok()));
    }
    out.push(s.summary("utf16_to_byte", name, bytes));
    let lines = buffer.line_count();
    let mut s = Scenario::default();
    for _ in 0..ops {
        let line = rng.below(lines);
        black_box(s.run(|| buffer.line_text(line).ok().map(|l| l.len())));
    }
    out.push(s.summary("line_lookup", name, bytes));
}

/// Full parse vs. incremental reparse after a one-character edit.
fn parse_scenarios(name: &str, text: &str, ops: usize, out: &mut Vec<Value>) {
    let Ok(mut parser) = SyntaxParser::new() else {
        return;
    };
    let mut buffer = TextBuffer::new(text);
    let bytes = text.len();
    let mut s = Scenario::default();
    let mut tree = None;
    for _ in 0..5 {
        tree = s.run(|| parser.parse(&buffer, None, &|| false).ok().flatten());
    }
    out.push(s.summary("parse_full", name, bytes));
    let Some(mut tree) = tree else { return };

    let mut rng = Rng(0xBEEF);
    let mut s = Scenario::default();
    for _ in 0..ops {
        // Insert a space right after a newline (always valid Rust).
        let at = boundary(&buffer, rng.below(buffer.len_bytes()));
        let line = buffer.byte_to_point(at).map(|p| p.line).unwrap_or(0);
        let Ok(start) = buffer.point_to_byte(titan_text::Point::new(line, 0)) else {
            continue;
        };
        let before = buffer.clone();
        let Ok(applied) = buffer.apply(vec![TextEdit::insert(start, " ")]) else {
            continue;
        };
        let next = s.run(|| {
            for edit in input_edits(&before, &applied.forward) {
                tree.edit(&edit);
            }
            parser.parse(&buffer, Some(&tree), &|| false).ok().flatten()
        });
        if let Some(next) = next {
            tree = next;
        }
    }
    out.push(s.summary("parse_incremental_after_edit", name, bytes));
}

fn search_scenarios(ops: usize, out: &mut Vec<Value>) {
    let Ok(dir) = tempfile::tempdir() else { return };
    let body = rust_source(100 * 1024);
    let files = 100;
    let src = dir.path().join("src");
    if fs::create_dir_all(&src).is_err() {
        return;
    }
    for i in 0..files {
        if fs::write(src.join(format!("m{i}.rs")), &body).is_err() {
            return;
        }
    }
    let Ok(workspace) = Workspace::open(dir.path()) else {
        return;
    };
    let overlays = Default::default();
    let mut s = Scenario::default();
    for _ in 0..(ops / 100).max(5) {
        black_box(s.run(|| {
            workspace
                .search(&SearchQuery::literal("function_777("), &overlays)
                .ok()
        }));
    }
    out.push(s.summary(
        "workspace_search",
        "workspace_100x100kb",
        files * body.len(),
    ));
    let mut s = Scenario::default();
    for _ in 0..(ops / 100).max(5) {
        black_box(s.run(|| workspace.list_files(10_000)));
    }
    out.push(s.summary(
        "workspace_list_files",
        "workspace_100x100kb",
        files * body.len(),
    ));
}

fn journal_scenarios(ops: usize, out: &mut Vec<Value>) {
    let (Ok(state), Ok(work)) = (tempfile::tempdir(), tempfile::tempdir()) else {
        return;
    };
    let path = work.path().join("doc.rs");
    let text = rust_source(1024 * 1024);
    if fs::write(&path, &text).is_err() {
        return;
    }
    let Ok(mut doc) = Document::open(&path, &Limits::default()) else {
        return;
    };
    let config = JournalConfig::new(state.path().to_path_buf());
    let Ok(mut journal) =
        JournalWriter::create(&config, BaseInfo::from_document(&doc), doc.text().buffer())
    else {
        return;
    };
    let mut rng = Rng(0x10A);
    for (name, sync) in [("journal_append", false), ("journal_append_fsync", true)] {
        let mut s = Scenario::default();
        let count = if sync { (ops / 10).max(20) } else { ops };
        for _ in 0..count {
            let at = boundary(
                doc.text().buffer(),
                rng.below(doc.text().buffer().len_bytes()),
            );
            let rev = doc.text().revision();
            let Ok(change) = doc.text_mut().apply(rev, vec![TextEdit::insert(at, "x")]) else {
                continue;
            };
            let _ = s.run(|| {
                let appended = journal.append(&change);
                if sync {
                    journal.sync().and(appended)
                } else {
                    appended
                }
            });
        }
        out.push(s.summary(name, "code_rust_1mb", text.len()));
    }
}

/// Content fingerprints: FNV-1a (64-bit, byte at a time) vs BLAKE3 (256-bit) on the same
/// bytes, and a conflict-safe save of a 1 MB document (validate + write + swap + verify).
fn hash_and_save_scenarios(ops: usize, out: &mut Vec<Value>) {
    let mb = 1024 * 1024;
    for (dataset, size) in [("bytes_1mb", mb), ("bytes_10mb", 10 * mb)] {
        let data = code_text(size, 11).into_bytes();
        let count = if size > mb {
            (ops / 20).max(10)
        } else {
            (ops / 4).max(20)
        };
        let mut s = Scenario::default();
        for _ in 0..count {
            black_box(s.run(|| titan_doc::stable_hash(black_box(&data))));
        }
        out.push(s.summary("hash_fnv1a", dataset, data.len()));
        let mut s = Scenario::default();
        for _ in 0..count {
            black_box(s.run(|| titan_doc::ContentHash::of(black_box(&data))));
        }
        out.push(s.summary("hash_blake3", dataset, data.len()));
    }
    let Ok(dir) = tempfile::tempdir() else { return };
    let path = dir.path().join("save.rs");
    let text = rust_source(mb);
    if fs::write(&path, &text).is_err() {
        return;
    }
    let Ok(mut doc) = Document::open(&path, &Limits::default()) else {
        return;
    };
    let mut s = Scenario::default();
    let mut strategy = String::new();
    for _ in 0..(ops / 20).max(20) {
        let rev = doc.text().revision();
        if doc
            .text_mut()
            .apply(rev, vec![TextEdit::insert(ByteOffset(0), "x")])
            .is_err()
        {
            break;
        }
        if let Ok(outcome) = s.run(|| doc.save(rev + 1, false)) {
            strategy = outcome.strategy.as_str().to_string();
        }
    }
    let mut summary = s.summary("document_save_safe", "code_rust_1mb", text.len());
    summary["strategy"] = json!(strategy);
    out.push(summary);
}

/// Round trips through a real `titand` over pipes.
fn protocol_scenarios(server: &Path, ops: usize, out: &mut Vec<Value>) {
    use titan_server::client::Client;
    let Ok(dir) = tempfile::tempdir() else { return };
    let text = code_text(1024 * 1024, 7);
    if fs::write(dir.path().join("big.txt"), &text).is_err() {
        return;
    }
    let Ok(mut client) = Client::spawn_with_args(server, &["--no-recovery"]) else {
        eprintln!("titan-bench: cannot start {}", server.display());
        return;
    };
    let ok = client
        .call(
            "session.hello",
            json!({ "protocolVersion": 1, "positionEncodings": ["utf-8"] }),
        )
        .and_then(|_| {
            client.call(
                "workspace.open",
                json!({ "path": dir.path().display().to_string() }),
            )
        })
        .and_then(|_| client.call("document.open", json!({ "path": "big.txt" })));
    let Ok(info) = ok else { return };
    let id = info["documentId"].as_u64().unwrap_or(0);

    let mut s = Scenario::default();
    for _ in 0..ops {
        black_box(s.run(|| client.call("system.health", Value::Null).ok()));
    }
    out.push(s.summary("rpc_health_roundtrip", "titand_stdio", 0));

    let mut s = Scenario::default();
    for revision in 0..ops as u64 {
        let params = json!({ "documentId": id, "expectedRevision": revision, "edits": [
            { "range": { "start": { "line": 100, "character": 0 },
                         "end": { "line": 100, "character": 0 } }, "text": "x" } ] });
        black_box(s.run(|| client.call("document.applyEdits", params).ok()));
    }
    out.push(s.summary("rpc_apply_edit_roundtrip", "titand_stdio_1mb", text.len()));
    let _ = client.shutdown();
}

/// `VmRSS` and `VmHWM` in KiB from `/proc/self/status` (Linux only).
fn memory_kib() -> Value {
    let Ok(status) = fs::read_to_string("/proc/self/status") else {
        return Value::Null;
    };
    let field = |name: &str| -> Option<u64> {
        status
            .lines()
            .find(|l| l.starts_with(name))
            .and_then(|l| l.split_whitespace().nth(1))
            .and_then(|v| v.parse().ok())
    };
    json!({ "rssKib": field("VmRSS:"), "peakRssKib": field("VmHWM:") })
}

fn cpu_model() -> Option<String> {
    let info = fs::read_to_string("/proc/cpuinfo").ok()?;
    info.lines()
        .find(|l| l.starts_with("model name"))
        .and_then(|l| l.split_once(':'))
        .map(|(_, v)| v.trim().to_string())
}

fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    let flag_value = |flag: &str| {
        args.iter()
            .position(|a| a == flag)
            .and_then(|i| args.get(i + 1).cloned())
    };
    let quick = args.iter().any(|a| a == "--quick");
    let out_path = flag_value("--out");
    let server = flag_value("--server");
    let rust_analyzer = flag_value("--rust-analyzer");
    let profile_server = flag_value("--profile-server");
    let ops = if quick { 200 } else { 2_000 };
    let mb = 1024 * 1024;

    let datasets: Vec<(&str, String)> = vec![
        ("code_1mb", code_text(mb, 1)),
        ("code_10mb", code_text(10 * mb, 2)),
        ("single_line_1mb", single_line(mb)),
        ("unicode_mixed_1mb", unicode_text(mb, 3)),
    ];

    let mut results = Vec::new();
    let mut memory = Vec::new();
    memory.push(json!({ "after": "start", "memory": memory_kib() }));
    for (name, text) in &datasets {
        eprintln!("titan-bench: text {name} ({} bytes)", text.len());
        text_scenarios(name, text, ops, &mut results);
        memory.push(json!({ "after": name, "memory": memory_kib() }));
    }
    for (name, size) in [("code_rust_100kb", 100 * 1024), ("code_rust_1mb", mb)] {
        eprintln!("titan-bench: parse {name}");
        parse_scenarios(name, &rust_source(size), (ops / 10).max(20), &mut results);
    }
    memory.push(json!({ "after": "parse", "memory": memory_kib() }));
    eprintln!("titan-bench: search");
    search_scenarios(ops, &mut results);
    eprintln!("titan-bench: hashing and saving");
    hash_and_save_scenarios(ops, &mut results);
    eprintln!("titan-bench: recovery journal");
    journal_scenarios(ops, &mut results);
    if let Some(server) = &server {
        eprintln!("titan-bench: protocol via {server}");
        protocol_scenarios(Path::new(server), (ops / 4).max(50), &mut results);
    }
    let system = match &server {
        Some(server) if !args.iter().any(|a| a == "--no-load") => load::run(
            Path::new(server),
            rust_analyzer.as_deref().map(Path::new),
            (ops / 4).max(100),
            &mut results,
        ),
        _ => Value::Null,
    };
    let profile = match &server {
        Some(server) if !args.iter().any(|a| a == "--no-load") => profile::run(
            profile_server.as_deref().map(Path::new),
            Path::new(server),
            rust_analyzer.as_deref().map(Path::new),
        ),
        _ => Value::Null,
    };
    memory.push(json!({ "after": "all", "memory": memory_kib() }));

    let report = json!({
        "tool": "titan-bench",
        "version": env!("CARGO_PKG_VERSION"),
        "profile": if cfg!(debug_assertions) { "debug" } else { "release" },
        "rustc": env::var("TITAN_RUSTC_VERSION").ok(),
        "gitCommit": env::var("GITHUB_SHA").ok(),
        "os": env::consts::OS,
        "arch": env::consts::ARCH,
        "cpu": cpu_model(),
        "cpus": std::thread::available_parallelism().map(|n| n.get()).unwrap_or(0),
        "opsPerScenario": ops,
        "seeds": { "code_1mb": 1, "code_10mb": 2, "unicode_mixed_1mb": 3,
                   "operations": "0xA11CE", "parse": "0xBEEF", "journal": "0x10A" },
        "allocator": "stats_alloc instrumented System allocator",
        "note": "Single-run wall-clock samples from std::time::Instant on one machine; \
                 they include timer and allocation-counting overhead and are not \
                 comparable across machines or with other editors.",
        "memory": memory,
        "system": system,
        "profile": profile,
        "results": results,
    });
    let text = serde_json::to_string_pretty(&report).unwrap_or_default();
    match out_path {
        Some(path) => {
            if let Err(err) = fs::write(&path, &text) {
                eprintln!("titan-bench: cannot write {path}: {err}");
                std::process::exit(1);
            }
            eprintln!("titan-bench: wrote {path}");
        }
        None => println!("{text}"),
    }
}
