# Dependency policy

Checked in CI by `cargo deny` (`deny.toml`, job "cargo deny"):

- **Advisories:** any RustSec advisory or yanked version in the dependency tree fails CI.
  To proceed with an advisory deliberately, add its id to `advisories.ignore` with a comment
  saying why it does not apply.
- **Licenses:** the allow list is exactly the set of licenses the current tree uses
  (permissive: MIT, Apache-2.0, BSD, ISC, Zlib, CC0, Unicode-3.0, Unlicense). It does not
  forbid anything that was approved before; it makes a *new* license visible in review.
  Workspace crates are `publish = false` and not checked.
- **Bans:** duplicate versions are warnings; wildcard version requirements are refused
  (path dependencies inside the workspace are allowed).
- **Sources:** crates.io only; no unknown registries or git dependencies.

Reproducibility: the toolchain is pinned (`rust-toolchain.toml`, `RUST_TOOLCHAIN` in CI)
and every CI run uploads its resolved `Cargo.lock` with the probe report.
