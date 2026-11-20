//! `titan-probe`: run the headless vertical slice against a real `titand` process and print
//! an auditable JSON report. Exit status is non-zero if any step fails.
//!
//! Usage: titan-probe [--server PATH] [--report PATH]

use std::env;
use std::fs;
use std::path::PathBuf;
use std::process::ExitCode;

use titan_server::probe::run_vertical_slice;

fn default_server() -> PathBuf {
    let name = format!("titand{}", env::consts::EXE_SUFFIX);
    env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(|dir| dir.join(&name)))
        .unwrap_or_else(|| PathBuf::from(name))
}

fn main() -> ExitCode {
    let args: Vec<String> = env::args().skip(1).collect();
    let mut server: Option<PathBuf> = None;
    let mut report_path: Option<PathBuf> = None;
    let mut i = 0;
    while i < args.len() {
        let value = args.get(i + 1).map(PathBuf::from);
        match args[i].as_str() {
            "--server" => server = value,
            "--report" => report_path = value,
            "-h" | "--help" => {
                println!("usage: titan-probe [--server PATH] [--report PATH]");
                return ExitCode::SUCCESS;
            }
            other => {
                eprintln!("titan-probe: unknown argument {other:?}");
                return ExitCode::from(2);
            }
        }
        i += 2;
    }
    let server = server.unwrap_or_else(default_server);

    let project = match tempfile::tempdir() {
        Ok(dir) => dir,
        Err(err) => {
            eprintln!("titan-probe: cannot create a temporary project: {err}");
            return ExitCode::FAILURE;
        }
    };
    let report = run_vertical_slice(&server, project.path());

    for step in &report.steps {
        println!("[{}] {}", if step.ok { " ok " } else { "FAIL" }, step.name);
    }
    let json = match serde_json::to_string_pretty(&report) {
        Ok(json) => json,
        Err(err) => {
            eprintln!("titan-probe: cannot serialise the report: {err}");
            return ExitCode::FAILURE;
        }
    };
    if let Some(path) = report_path {
        if let Err(err) = fs::write(&path, &json) {
            eprintln!("titan-probe: cannot write {}: {err}", path.display());
            return ExitCode::FAILURE;
        }
        println!("report written to {}", path.display());
    } else {
        println!("{json}");
    }
    if report.ok {
        println!("vertical slice: PASS");
        ExitCode::SUCCESS
    } else {
        println!("vertical slice: FAIL");
        ExitCode::FAILURE
    }
}
