# Upstream code reuse register

Records every piece of source code copied or adapted from another editor into this repository:
upstream repo, commit, path, SPDX license, destination, modifications.

## Copied or adapted code
None so far. All code in `crates/` was written for TITAN Code. Designs studied in
`research/` (Zed, VS Code, Helix, Lapce, Neovim at the commits pinned there) informed the
decisions in `docs/decisions/`, but no upstream source was copied.

## Third-party crates (dependencies, not copied code)

| Crate | Used by | License (per crates.io metadata; verify with `cargo deny` when added) |
|---|---|---|
| ropey 1.6 | titan-text | MIT |
| unicode-segmentation 1 | titan-text | MIT OR Apache-2.0 |
| thiserror 2 | all | MIT OR Apache-2.0 |
| tempfile 3 | titan-doc, titan-server | MIT OR Apache-2.0 |
| ignore 0.4 | titan-workspace | Unlicense OR MIT |
| serde 1, serde_json 1 | titan-protocol, titan-server | MIT OR Apache-2.0 |

Automated license and advisory checking (`cargo deny`) is not set up yet.
