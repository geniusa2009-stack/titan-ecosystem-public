# Build and test notes - 20 November 2026

## Environment and method
Apple arm64, macOS 26.5.1; Rust 1.91.0 in a task-local toolchain; Qt 6.12.0; CMake/Ninja Release build with two workers. Private source repositories were read and copied. No original branch, project files or Unreal installation were modified. Build outputs, demo projects and toolchain files remain outside the showcase repository.

## TITAN Code
Selected Phase 4 source comprises 13 Rust crates: text, document, workspace, protocol, syntax index, tasks, recovery, watching, server, LSP, changes, benchmarks and verification. The core is headless; no frontend exists.

`cargo test --workspace` completed with **266 passed, 0 failed, 8 ignored**. Ignored tests need external language-server, small-filesystem or opt-in end-to-end fixtures; they are not passes. The first run failed two verification tests because rustfmt was absent. Installing the required toolchain component resolved both; the full suite was rerun. Both logs are retained.

The actual `titan-probe` executable drove the actual `titand` process with JSON-RPC: **22/22 steps passed**. This includes edit/read, stale-revision rejection, undo/redo, persistence, in-buffer and disk search, tree-sitter symbols, and recovery after killing the service. The expected stale-revision error in stderr is a successful negative test. A final unrestricted run used TITAN_STATE_DIR inside the task workspace. Local semantic rust-analyzer integration was not exercised by this probe.

Commands (from the appropriate source directory):
```
cargo test --workspace
TITAN_STATE_DIR=<isolated-state> <build>/titan-probe --report <report.json>
```
The diagnostic transcript records the headless service session.

## TITAN Studio
Selected latest checkpoint compiled successfully with CMake/Ninja and installed Qt components. Explicit Qt module paths were needed for the Homebrew layout. No source changes were made.

`ctest --test-dir <build> -LE unreal -E studio.ui-smoke --timeout 60 --output-on-failure`: **25/25 passed**. A preliminary sandboxed run failed IPC/frame-channel and GUI tests; the unrestricted rerun passed. Both outcomes are retained.

Separate real GUI run:
```
TITAN Studio --engine reference --smoke-test <new-demo-project> --screenshot-dir <captures> --report <report.json>
```
**60 checks passed, 1 skipped**. The skipped check is remote engine-rendered frames, which does not apply to the reference backend. The visible scene is really rendered by Studio's Metal viewport. This is not Unreal Engine footage. Evidence includes selecting and editing objects, undo, hierarchy operations, copy/paste, bookmarks, assets, procedural preview and save/reopen.

Unreal tests were excluded: free space was about 7.5 GiB initially, below the project's 35/40 GB runtime/build guards. No Unreal build, plugin sync or engine launch was attempted. New headless core modules (physics/audio/navigation/AI etc.) being unit-tested does not imply that all are exposed in the editor or production-ready.

The supplied MP4 is a 60-second 1920x1080 H.264 sequence of six genuine application screenshots with English captions, not continuous video. Original PNGs remain unretouched.

## Codex
Next.js 15.2.0 / React 19 frontend. Dependencies installed from the existing lockfile. The app ran on 127.0.0.1:3177. `tsc --noEmit --incremental false` exited 0. Browser checks: dashboard rendered; My Courses navigated to the catalog; View Course opened a lesson; Check Answer was disabled before selection, enabled after selecting `x = 5`, and displayed the green check afterward. Four 1920x1080 screenshots preserve these states.

The quiz demonstrates client-side state, not an assessment backend. The source always reveals the correct option after submission; chat sending, lesson tabs, hint, course filters and several navigation targets lack complete behavior. Dashboard XP, streak, achievements and course progress are hardcoded sample data; no learning outcomes or user counts are claimed. No authentication, database persistence, AI tutor, code execution or TITAN integration was verified. The upstream Next.js version produced an installation security warning; it is preserved for authenticity and was used only on loopback. No deployment or additional hosting was created.

## Reading the test records
Historical CI artifacts are explicitly separate from this local run. Their metrics are not new benchmarks. Source review, a compiler check, tests and a GUI session provide different levels of evidence; none alone proves a finished application.
