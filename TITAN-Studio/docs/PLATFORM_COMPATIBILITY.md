# Platform Compatibility

| Component | macOS arm64 | macOS x64 | Windows x64 | Linux x64 |
|---|---|---|---|---|
| titan_core / engine host / tests | **Verified** (M3, macOS 26.5.1) | Unverified | Not implemented (IPC, dlopen) | Unverified (POSIX code paths written) |
| Procedural scatter extension | **Verified** | Unverified | Not implemented | Unverified |
| TITAN Studio | **Verified** (Metal) | Unverified | Unverified (would use D3D11/12 via QRhi; host transport missing) | Unverified (Vulkan/GL via QRhi) |
| TitanStudioBridge (UE) | Not compiled | Not compiled | Not supported (transport) | Not compiled |
| TITAN v0.5 portable core | **Verified** | — | — | CI config existed in v0.5 (not run here) |

A platform is marked supported only after a build plus test run on that platform.
