//! `titand`: the TITAN Code headless service. JSON-RPC 2.0 with Content-Length framing on
//! stdin/stdout; diagnostics on stderr. Not a network service.
//!
//! Usage: titand [--state-dir PATH | --no-recovery] [--no-watch] [--workers N]
//!               [--rust-analyzer PATH | --no-lsp] [--lsp-arg ARG]... [--lsp-settings JSON]
//!               [--lsp-timeout-ms N] [--lsp-name NAME]
//!
//! The language server is optional and must be named explicitly: without `--rust-analyzer`
//! semantic queries answer from the syntax index and say so. `--lsp-arg` passes arguments
//! to the server; `--lsp-settings` replaces the default light rust-analyzer settings.
//!
//! Recovery journals go to `--state-dir`, else `$TITAN_STATE_DIR`, else a per-user default
//! (`$XDG_STATE_HOME/titan-code`, `~/.local/state/titan-code` on Linux,
//! `~/Library/Application Support/TITAN Code` on macOS). Without any of these, recovery is
//! disabled and the server says so in `session.hello`.

use std::env;
use std::io::{self, BufReader, BufWriter};
use std::path::PathBuf;
use std::process::ExitCode;

use titan_protocol::PROTOCOL_VERSION;
use titan_server::{LspOptions, ServerOptions, run};

#[cfg(feature = "alloc-stats")]
#[global_allocator]
static GLOBAL: &stats_alloc::StatsAlloc<std::alloc::System> = &stats_alloc::INSTRUMENTED_SYSTEM;

/// Live heap of the counting allocator (profiling builds only).
#[cfg(feature = "alloc-stats")]
fn heap_stats() -> serde_json::Value {
    let s = GLOBAL.stats();
    let live =
        s.bytes_allocated as i128 - s.bytes_deallocated as i128 + s.bytes_reallocated as i128;
    serde_json::json!({
        "liveBytes": live,
        "allocations": s.allocations,
        "deallocations": s.deallocations,
        "reallocations": s.reallocations,
        "bytesAllocatedTotal": s.bytes_allocated,
        "note": "Rust global allocator only; tree-sitter (C) allocations and allocator overhead are not included",
    })
}

fn default_state_dir() -> Option<PathBuf> {
    if let Some(dir) = env::var_os("TITAN_STATE_DIR") {
        return Some(PathBuf::from(dir));
    }
    let home = env::var_os("HOME").map(PathBuf::from);
    if cfg!(target_os = "macos") {
        return home.map(|h| h.join("Library/Application Support/TITAN Code"));
    }
    if let Some(state) = env::var_os("XDG_STATE_HOME") {
        return Some(PathBuf::from(state).join("titan-code"));
    }
    home.map(|h| h.join(".local/state/titan-code"))
}

fn main() -> ExitCode {
    let args: Vec<String> = env::args().skip(1).collect();
    let mut options = ServerOptions {
        state_dir: default_state_dir(),
        ..ServerOptions::default()
    };
    let mut lsp: Option<LspOptions> = None;
    let mut lsp_args: Vec<String> = Vec::new();
    let mut lsp_settings: Option<serde_json::Value> = None;
    let mut lsp_timeout: Option<u64> = None;
    let mut lsp_name: Option<String> = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--state-dir" => {
                let Some(dir) = args.get(i + 1) else {
                    eprintln!("titand: --state-dir needs a path");
                    return ExitCode::from(2);
                };
                options.state_dir = Some(PathBuf::from(dir));
                i += 1;
            }
            "--no-recovery" => options.state_dir = None,
            "--no-watch" => options.watch = false,
            "--no-graph" => options.graph = false,
            "--graph-max-files" | "--simulate-memory-pressure" => {
                let flag = args[i].as_str();
                let Some(value) = args.get(i + 1) else {
                    eprintln!("titand: {flag} needs a value");
                    return ExitCode::from(2);
                };
                if flag == "--graph-max-files" {
                    match value.parse::<usize>() {
                        Ok(n) => options.graph_max_files = n,
                        Err(_) => {
                            eprintln!("titand: --graph-max-files needs a number");
                            return ExitCode::from(2);
                        }
                    }
                } else {
                    options.simulated_pressure = Some(value.clone());
                }
                i += 1;
            }
            "--no-lsp" => lsp = None,
            "--rust-analyzer" | "--lsp-arg" | "--lsp-settings" | "--lsp-timeout-ms"
            | "--lsp-name" => {
                let flag = args[i].as_str();
                let Some(value) = args.get(i + 1) else {
                    eprintln!("titand: {flag} needs a value");
                    return ExitCode::from(2);
                };
                match flag {
                    "--rust-analyzer" => {
                        lsp = Some(LspOptions::rust_analyzer(PathBuf::from(value)))
                    }
                    "--lsp-arg" => lsp_args.push(value.clone()),
                    "--lsp-name" => lsp_name = Some(value.clone()),
                    "--lsp-settings" => match serde_json::from_str(value) {
                        Ok(v) => lsp_settings = Some(v),
                        Err(e) => {
                            eprintln!("titand: --lsp-settings is not JSON: {e}");
                            return ExitCode::from(2);
                        }
                    },
                    _ => match value.parse::<u64>() {
                        Ok(ms) => lsp_timeout = Some(ms.max(1)),
                        Err(_) => {
                            eprintln!("titand: --lsp-timeout-ms needs a number");
                            return ExitCode::from(2);
                        }
                    },
                }
                i += 1;
            }
            "--workers" => {
                let Some(n) = args.get(i + 1).and_then(|n| n.parse::<usize>().ok()) else {
                    eprintln!("titand: --workers needs a number");
                    return ExitCode::from(2);
                };
                options.workers = n.max(1);
                i += 1;
            }
            "-h" | "--help" => {
                println!(
                    "usage: titand [--state-dir PATH | --no-recovery] [--no-watch] [--workers N] \
                     [--rust-analyzer PATH | --no-lsp] [--lsp-arg ARG]... [--lsp-settings JSON] \
                     [--lsp-timeout-ms N] [--lsp-name NAME]"
                );
                return ExitCode::SUCCESS;
            }
            other => {
                eprintln!("titand: unknown argument {other:?}");
                return ExitCode::from(2);
            }
        }
        i += 1;
    }
    if let Some(lsp) = lsp.as_mut() {
        lsp.args = lsp_args;
        if let Some(name) = lsp_name {
            lsp.name = name;
        }
        if let Some(settings) = lsp_settings {
            lsp.settings = settings;
            lsp.light_mode = false;
        }
        if let Some(ms) = lsp_timeout {
            lsp.request_timeout = std::time::Duration::from_millis(ms);
        }
    }
    options.lsp = lsp;
    #[cfg(feature = "alloc-stats")]
    {
        options.heap_stats = Some(heap_stats);
    }
    eprintln!(
        "titand {} ready (protocol v{PROTOCOL_VERSION}, stdio, {} workers, recovery {})",
        env!("CARGO_PKG_VERSION"),
        options.workers,
        options
            .state_dir
            .as_ref()
            .map_or_else(|| "disabled".to_string(), |d| format!("in {}", d.display()))
    );
    let reader = BufReader::new(io::stdin());
    let writer = BufWriter::new(io::stdout());
    match run(reader, writer, options) {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("titand: {err}");
            ExitCode::FAILURE
        }
    }
}
