//! `system.memory`: where the memory of `titand` goes.
//!
//! Three kinds of numbers, never mixed:
//! * **process** — what the OS charges (`/proc/self/status` and `smaps_rollup` on Linux:
//!   resident, anonymous, file-backed, shared, peak). `null` on other platforms.
//! * **heap** — live bytes of the Rust global allocator, only in builds with the
//!   `alloc-stats` feature (CI's profiling build). tree-sitter's C allocations are not
//!   included; neither is allocator overhead.
//! * **estimates** — sizes computed from TITAN's own data structures (text, undo history,
//!   index, graph, journals, language-server state). Lower bounds, labelled as estimates.
//!
//! The language server is a separate process; its memory is reported separately (`lsp`).

use std::fs;

use serde_json::{Value, json};

use super::Session;

fn kib_fields(text: &str, names: &[&str]) -> Value {
    let mut out = serde_json::Map::new();
    for name in names {
        let value = text
            .lines()
            .find(|l| l.starts_with(&format!("{name}:")))
            .and_then(|l| l.split_whitespace().nth(1))
            .and_then(|v| v.parse::<u64>().ok());
        out.insert(name.to_string(), json!(value));
    }
    Value::Object(out)
}

/// Memory of process `pid` (`"self"` for this one) from `/proc`, in KiB.
pub(crate) fn proc_memory(pid: &str) -> Value {
    let Ok(status) = fs::read_to_string(format!("/proc/{pid}/status")) else {
        return Value::Null;
    };
    let mut out = kib_fields(
        &status,
        &[
            "VmRSS", "RssAnon", "RssFile", "RssShmem", "VmHWM", "VmSize", "VmSwap",
        ],
    );
    if let Ok(rollup) = fs::read_to_string(format!("/proc/{pid}/smaps_rollup")) {
        out["smaps"] = kib_fields(
            &rollup,
            &[
                "Rss",
                "Pss",
                "Shared_Clean",
                "Shared_Dirty",
                "Private_Clean",
                "Private_Dirty",
            ],
        );
    }
    out["threads"] = json!(
        status
            .lines()
            .find(|l| l.starts_with("Threads:"))
            .and_then(|l| l.split_whitespace().nth(1))
            .and_then(|v| v.parse::<u64>().ok())
    );
    out
}

impl Session {
    pub(super) fn memory_report(&self) -> Value {
        let mut text_bytes = 0usize;
        let mut history_bytes = 0usize;
        let mut undo_entries = 0usize;
        let mut tree_nodes = 0usize;
        let mut trees = 0usize;
        let mut syntax_diagnostics = 0usize;
        for od in self.documents.values() {
            text_bytes += od.doc.text().buffer().len_bytes();
            history_bytes += od.doc.text().history_bytes();
            undo_entries += od.doc.text().undo_depth();
            if let Some(tree) = &od.syntax.tree {
                trees += 1;
                tree_nodes += tree.root_node().descendant_count();
            }
            syntax_diagnostics += od.syntax.diagnostics.as_ref().map_or(0, |(_, d)| d.len());
        }
        let journal_bytes: u64 = self
            .documents
            .values()
            .filter_map(|od| {
                od.journal
                    .as_ref()
                    .map(titan_recovery::JournalWriter::len_bytes)
            })
            .sum();
        let diagnostics_items: usize = self
            .semantic
            .diagnostics
            .values()
            .map(|d| d.items.len())
            .sum();
        let lsp_pid = self
            .semantic
            .supervisor
            .as_ref()
            .and_then(titan_lsp::Supervisor::pid);
        json!({
            "process": proc_memory("self"),
            "resources": self.resources_json(),
            "heap": self.options.heap_stats.map(|f| f()),
            "estimates": {
                "note": "computed from TITAN data structures; lower bounds, not allocator measurements",
                "documents": {
                    "open": self.documents.len(),
                    "textBytes": text_bytes,
                    "undoHistoryBytes": history_bytes,
                    "undoEntries": undo_entries,
                },
                "syntaxTrees": {
                    "trees": trees,
                    "nodes": tree_nodes,
                    "note": "tree-sitter allocates in C; node counts only",
                    "diagnostics": syntax_diagnostics,
                },
                "index": {
                    "files": self.index.stats().files,
                    "symbols": self.index.stats().symbols,
                    "estimatedBytes": self.index.estimated_bytes(),
                },
                "graph": {
                    "generation": self.graph.graph.as_ref().map_or(0, |g| g.generation()),
                    "estimatedBytes": self.graph.graph.as_ref().map_or(0, |g| g.estimated_bytes()),
                    "facts": self.graph_cache_stats(),
                },
                "journals": { "active": self.documents.values().filter(|od| od.journal.is_some()).count(), "bytesOnDisk": journal_bytes },
                "languageServer": {
                    "storedDiagnostics": diagnostics_items,
                    "pendingRequests": self.semantic.pending.len(),
                    "syncedDocuments": self.semantic.synced.len(),
                },
                "tasks": {
                    "queued": self.pool.metrics().queued,
                    "running": self.pool.metrics().running,
                    "pending": self.pending.len(),
                },
                "replacedVersionsWatched": self.displaced.len(),
            },
            "languageServerProcess": lsp_pid.map(|pid| proc_memory(&pid.to_string())),
        })
    }
}
