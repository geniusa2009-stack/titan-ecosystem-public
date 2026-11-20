# TITAN verification ledger

Each row records what was proven, how, and where the evidence is. Status levels: implemented, compiled, tested,
integrated, benchmark-verified, release-ready. They are not interchangeable. Logs live in `~/TitanEngine/logs` and
`~/TitanEngine/sessions` (local only); the details are in `docs/TEST_REPORT.md`.

| Date | Item | Status | Evidence |
|---|---|---|---|
| 2026-11-06 | UE 5.8.4 dependencies (98,381 files, 33.5 GB) | tested | SHA-1 of every selected file matches the manifest (re-verified 2026-11-09, 0 bad) |
| 2026-11-06 | TitanStudioBridge + TitanHost compile against UE 5.8.4 | compiled | `20261106T203116Z-bridge-check.log`, 6 objects, 0 errors |
| 2026-11-09 | Full `TitanHostEditor` build | compiled + linked | `20261109T121208Z-build.log`, exit 0, 1,609 actions, arm64 binaries, bridge dylib dependencies resolve |
| 2026-11-09 | `ShaderCompileWorker` built by `ue_build.sh build` | compiled | `20261109T135707Z-build.log`, exit 0; test `BuildStepTargets` (fails against the previous step) |
| 2026-11-09 | Bridge loads in UnrealEditor, protocol v1 | integrated | `TITAN_HOST_READY` in `TitanHost.log`; conformance step `session.hello` |
| 2026-11-09 | `ue.bridge-conformance` 18/18 | integrated | `…unreal-suite.LastTest.log`; frames in `docs/screenshots/unreal/frame-*.png` |
| 2026-11-09 | `ue.automation` 2/2 | integrated | same run |
| 2026-11-09 | Studio ↔ Unreal session (29 PASS / 3 SKIP) | integrated | sessions `20261109T142005Z`, `…145318Z-r2`, `…145507Z-r3`; `docs/screenshots/unreal/studio-*.png`; 1 failing run `…144922Z-r1` (open) |
| 2026-11-09 | Viewport resize crash fix (readback pool) | tested + integrated | the new conformance step reproduced the SIGSEGV on the unfixed bridge and passes with the fix |
| 2026-11-09 | Inspector numeric input (validator regex) | tested | smoke step `inspector number field accepts typed numbers`; fails with the old regex |
| 2026-11-09 | Plugin sync timestamps | tested | `PluginSync` (3 cases); 2 fail with `rsync -a`, 1 without `--checksum` |
| 2026-11-09 | Frame channel: seqlock fences, cached overflow-safe geometry | tested | `studio.frame-channel` 11/11 (PR #3 branch: 12/12); mutation checks in the PR #2/#3 comments; the fences themselves are not demonstrated by a test |
| 2026-11-09 | Engine frame rate and latency | measured (3 runs, not a benchmark suite) | 8.0 fps (6.8–9.3), GPU 217 ms (73–322), latency 670 ms (229–1099) at 802×828 |
| — | 60 fps Unreal viewport | not achieved | — |
| — | World Builder on Unreal | not implemented | the bridge lacks `worldbuilder.*`; reported as SKIP |
