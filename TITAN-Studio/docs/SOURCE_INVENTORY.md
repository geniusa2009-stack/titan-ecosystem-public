# Source Inventory

Inventory performed 2026-11-05 on the development Mac (Apple M3, 8 GB, macOS 26.5.1).
**V** = verified by inspection/command; **A** = assumption or claim not verifiable from supplied files.

## Supplied archives

| # | File | Size | SHA-256 | Contents | Location in workspace |
|---|------|------|---------|----------|-----------------------|
| 1 | `UnrealEngine-release.zip` | 769,391,210 B | `f07722c063772d63bc58be4f35195f0d004594e202e80d7d32475cfe36c7b187` | Epic Games Unreal Engine source, **version 5.8.4** (V: `Engine/Build/Build.version` → Major 5, Minor 8, Patch 4, Branch `UE5`, `IsLicenseeVersion: 0`). 256,039 zip entries, 2.87 GB uncompressed; 224,008 files. No `Engine/Content`, no git dependencies (`Setup.sh` not run). | `vault/extracted/ue-5.8.4/` (read-only) |
| 2 | `TITAN-UE427-Native-Integration-v0.5.zip` | 79,561 B | `0ec27747e1d0779f44f5e998392e9df23d2213333e32124a408f70b4dc6cb22f` | Prior TITAN deliverable v0.5: `TitanFoundation` UE plugin (4 modules), portable C++14 core, tests, benchmarks, docs, installer script. 64 files. | `vault/extracted/titan-v0.5/` (read-only), imported unmodified to `legacy/TitanFoundation-v0.5/` |

**Important discrepancy (V):** the project directive describes an *Unreal Engine 4.27* fork and refers to
`AlexMercer-MA/UnrealEngine-4.27`. The supplied archive is **UE 5.8.4**, not 4.27. TITAN v0.5's own
contract checker (`tools/verify_ue427_source_contract.py`) reports `"expected UE4.27"` against it, while all
UE API signatures it checks are present. See `SOURCE_INTEGRATION_DECISIONS.md` (D-1).

**Not supplied (V):** no partner/technology-company source archive, no signed agreements, no licence
or authorization documents, no TITAN Forge skill pack. Only the two archives above were accessible.

## Archive safety checks (V)

- No absolute paths or `..` components in either archive (`unzip -Z1 | grep`).
- No symlink entries in the UE archive.
- Extracted into a dedicated `vault/extracted/` tree, then made read-only (`chmod -R a-w`).
- Original TITAN archive copied to `vault/originals/`; the 770 MB UE zip remains at its upload location with
  the checksum above.
- No build script from either archive has been executed. (`Setup.sh` was read, not run: it downloads
  dependencies from Epic's CDN.)

## Secret scan (V, values not printed)

Patterns: RSA/OpenSSH/PKCS#8 private-key headers, AWS access keys, GitHub tokens, Slack tokens.

| Match | Assessment |
|---|---|
| `Engine/Source/ThirdParty/WebRTC/{4664,5414}/Include/pc/test/fake_rtc_certificate_generator.h` | Upstream WebRTC **test fixture** certificates. Not a credential. |
| `Engine/Source/Programs/UnrealBuildTool/Platform/Apple/RemoteMac.cs` | UBT code that handles SSH keys for remote Mac builds (matches the header text). Not a credential. |
| AWS / GitHub / Slack token patterns | 0 matches. |
| TITAN v0.5 archive | 0 matches. |

## UE 5.8.4 structure relevant to TITAN (V)

- `Engine/Source/{Runtime,Editor,Developer,Programs,ThirdParty}`; 195 Runtime module directories.
- Renderer stack present: `Runtime/RHI`, `RHICore`, `RenderCore`, `Renderer`, `Runtime/Apple/MetalRHI`.
- 162 ThirdParty directories; 862 `.tps` third-party notice files across the tree; 922 `.uplugin` files.
- Build: UnrealBuildTool (C#/.NET, `Engine/Binaries/DotNET`), `Setup.sh` → `GitDependencies` (downloads
  binaries), `GenerateProjectFiles.sh`, `Engine/Build/BatchFiles/Mac/Build.sh`.

## TITAN v0.5 contents (V)

| Component | Language / build | Status here |
|---|---|---|
| `TitanOrchestrationCore` (WorldScheduler, FrameGraph, TaskGraph, StreamingTelemetry) | C++14, CMake + UBT | Builds; tests pass (4/4 CTest) |
| `TitanDataCore` (ECS, FixedClock, CPU Visibility) | C++14 | Builds; tests pass |
| `TitanSpatialCore` (header-only planner) | C++14 | Tests pass |
| `TitanFoundationRuntime` (UE components incl. level-streaming bridge) | UE C++ | **Not compiled** (needs UE build) |
| `scripts/install_into_ue427.py` | Python | Not executed against the UE tree |
