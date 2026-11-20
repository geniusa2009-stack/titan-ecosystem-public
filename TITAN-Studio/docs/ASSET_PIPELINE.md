# Asset pipeline (Issue #5, milestone M1)

TITAN Core code (`core/include/titan/assets.h`), with no Qt or Unreal dependency.

## Layout and identity
- **Sources:** `Assets/…`. Each source gets a sidecar, `<file>.tmeta`, holding its stable 128-bit asset id and its importer settings.
- **Moves and renames:** moving or renaming a file together with its sidecar keeps the id. If the sidecar is lost, the source is re-associated with its previous id by SHA-256 of the content, provided the old path no longer exists.
- **Registry:** `Library/AssetDB.json`, written atomically. A corrupt or unknown-format registry is rebuilt; the ids survive in the sidecars.
- **Products:** content-addressed under `Library/Artifacts/<2 hex>/<key>.json`. A product is reused only if it parses and its stored key matches.

## Incremental rules
- **Hashing:** a source is hashed again only when its size or mtime changes.
- **Cache key:** `SHA-256("titan-asset-v1", importer id, importer version, settings, source hash, sorted hashes of its dependencies)`. It is independent of the asset id, so keys are identical across project copies.
- **Changes:** changing a source re-imports it and every asset that reads it (for example, a glTF that references the texture or `.bin`). Nothing else is re-imported.
- **Failures:** a failed import keeps the last valid product and records the error. Failed assets retry on every refresh, for example once a missing dependency appears.
- **Cancellation:** a cancel flag is polled between assets; the finished work is saved, and the next refresh completes the rest.
- **Deleted sources:** marked `missing`. Their product stays until `collectGarbage()`, which removes only unreferenced files inside `Library/Artifacts`.

## Importers (version 1)
| Importer | Formats | Validates | Product metadata |
|---|---|---|---|
| `model.gltf` | `.gltf`, `.glb` | GLB header, version 2, length field, JSON/BIN chunk bounds; glTF 2.0 `asset.version`; every buffer has data; bufferViews inside their buffers | meshes, nodes, materials, textures, images, animations, generator |
| `image.png` | `.png` | signature, chunk bounds, CRC-32 of every chunk, IHDR first, IDAT and IEND present, size ≤ 65536, valid color type / bit depth | width, height, channels, bit depth |
| `image.jpeg` | `.jpg`, `.jpeg` | SOI, segment bounds, a frame header before the scan | width, height, components, progressive |
| `audio.wav` | `.wav` | RIFF/WAVE, chunk bounds, PCM or float, consistent fmt fields | channels, sample rate, bits, frames, duration |
| `data.buffer` | `.bin` | — (tracked so models re-import when their buffers change) | size |

## Security
- Only regular files under `Assets/` are read; symlinks and hidden files are skipped and never followed.
- Sources above `Options::maxSourceBytes` (default 512 MiB) are refused without being read.
- References (glTF `uri`) are percent-decoded and must stay inside `Assets/`. Absolute paths, schemes, drive letters, backslashes and `..` escapes are rejected; `data:` URIs are allowed and not tracked.

## Evidence
- `core.assets` passes 16/16. It covers:
  - FIPS 180-4 test vectors;
  - real headers, plus malformed, truncated and oversized inputs;
  - incremental reuse, and change propagation to dependents only;
  - moves with and without the sidecar;
  - failure preservation;
  - missing and escaping dependencies;
  - symlinks and hidden files;
  - cancellation;
  - a corrupt artifact or registry;
  - deterministic keys, missing sources and garbage collection.
- Mutation-checked: removing failure preservation, the dependency hashes in the key, the artifact integrity check, the symlink skip, or the `..` escape check each fails the intended test.
- `titan-asset-bench --assets 2000 --runs 3` (Apple M3, 8 GB, 2,000 × 64 KiB PNG, 131 MB):

  | Operation | Time | Result |
  |---|---|---|
  | Cold import | 1.86–3.37 s | 2,000 imported |
  | No-change refresh | 145–223 ms | 0 hashed, 2,000 reused |
  | One file changed | 120–163 ms | exactly 1 imported |

  The machine had about 5.5 GB of swap in use, so the absolute numbers vary.

## Studio and protocol
- **Engine-host methods:** the reference engine host implements `assets.list` and `assets.refresh` (capability `assets`; a refresh emits the event `assets.changed`). The `host.integration` test covers them on a real host process.
- **Studio:** the **Assets** workspace (last in the sidebar) lists path, importer, id, kind summary, status, errors and dependency counts. It has a path filter and an *Import changes* action.
- **UI evidence:** the UI smoke test writes a WAV, a glTF and its `.bin` into the project, opens the workspace, imports them and requires 3 entries. Screenshot: `docs/screenshots/assets-workspace.png` (reference engine).

## Not done yet
- **Unreal backend:** the Unreal bridge does not advertise the `assets` capability; Studio's Assets workspace states this instead of showing an empty list.
- **Products are metadata only:** no texture compression, mesh conversion or platform-specific outputs.
- **Formats:** no MP3, OGG or FLAC, and no KTX2 or EXR.
- **Concurrency:** imports run sequentially; bounded parallelism waits for the job scheduler (milestone M3).
- **Background work:** no file watcher; refresh is explicit.
