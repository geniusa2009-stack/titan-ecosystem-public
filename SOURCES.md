# Sources and attribution

Source snapshots prepared on 20 November 2026. The original project repositories remain separate.

| Destination | Source repository | Source ref | Full commit |
|---|---|---|---|
| TITAN-Code | https://github.com/geniusa2009-stack/TITAN-Code | feat/phase4-project-twin | 55aeaa143ace4f1585eb13bb6badd27f0f67b9be |
| TITAN-Studio | https://github.com/geniusa2009-stack/TITAN | checkpoint/titan-2026-11-17-pr36 | ca1d1a10904d1c542c018a5993a26507c383a287 |
| Codex | https://github.com/geniusa2009-stack/codex-frontend | main | 0deea1235bffd5fabf86bb99b5e12156186291e3 |

## Selection
The selected source snapshots and their provenance are retained. Private branch inventories are not included in this release.

- Code: `main` is a four-file baseline. Phase 4 has verified complete CI run 42778350519, including macOS/Linux tests and a real probe artifact. Later Phase 5 and universal-intelligence branches exist; their newest runs are not green and were not chosen as the verified baseline. Historical CI: https://github.com/geniusa2009-stack/TITAN-Code/actions/runs/42778350519 . The selected source was also built and tested locally in this task.
- Studio: the latest checkpoint incorporates the feature stack through perception and the behavior-tree timeout fix. It was built and tested here. Older stage-preflight CI is historical evidence only, not proof of the selected checkpoint: https://github.com/geniusa2009-stack/TITAN/actions/runs/42660081293 .
- Codex: one remote branch. The local-source variant in `current/مصادر العمل المحلية/codex-frontend` was run. The ZIP-derived variant was preserved but not separately validated. Release ordering between variants is unknown.

## Attribution
Copyright and original notices remain with their respective authors/rightsholders. The original account is `geniusa2009-stack`. Selected Code and Studio commit messages include `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. These records establish AI assistance, not an exact division of human effort. Commit authorship is not independent proof of who implemented every line.

## Rights and exclusions
TITAN Code's README states proprietary/all rights reserved until a distribution license is chosen. Its reuse register reports no copied editor source; Rust dependencies retain their own licenses. Studio's rights matrix distinguishes original TITAN code and bridge code from restricted Unreal Engine source. No Unreal Engine tree or binaries were copied. `unreal/Plugins/TitanStudioBridge` is the original bridge source already present in the private TITAN repository. Original legacy notices and licensing documents remain intact. Qt and Rust/npm dependencies are referenced, not vendored or redistributed.

See PUBLIC_RELEASE.md for the public-release review scope and limitations. This release does not import any historical commits.

## Integrity
`docs/engineering/source-file-manifest.json` records all 466 copied source files, original Git blob IDs, SHA-256 digests and source commits. All matched at verification. The generated Rust Cargo.lock is not part of the source snapshot; it is preserved as `Cargo.local-validation.lock`. Historical CI's lockfile is stored separately with its downloaded artifacts.
