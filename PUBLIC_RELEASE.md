# Public release review

Prepared 20 November 2026 as an independent source release authorized by the repository owner. No historical Git objects, branches or tags were imported. This packaging commit does not claim authorship of the original code.

## Included

- TITAN Code, TITAN Studio and Codex source snapshots: all 466 files listed in `docs/engineering/source-file-manifest.json` match their recorded SHA-256 hashes.
- Original architecture, build, testing, provenance, copyright and rights documents.
- Authentic screenshots and the current 60-second Studio demonstration. The demonstration is a captioned screenshot sequence, not continuous footage.
- Reviewed technical logs and historical benchmark artifacts, identified as historical evidence rather than new test results.

## Excluded and adjusted

Personal narrative documents, private presentation material, the older presentation video, private branch inventories and all prior Git history are excluded. The overview PDF is not part of this release. Public-facing summaries were rewritten as technical descriptions; project functionality and source files were not changed. Links and the media hash inventory were updated for the release contents.

Original source repository references, source commit identifiers, team attribution and documented Claude co-author attribution remain in `SOURCES.md`. References are provenance records, not imported commits. The original private repositories remain separate and unchanged.

## Security and privacy review

- Gitleaks scanned the complete release directory with zero detected leaks. The new Git history is also scanned before publication.
- Text and filenames were checked for credentials, application material, personal contact details and identifying local user paths. No publication blockers were found. Example-domain addresses and hardcoded demo profile data are test/sample content; co-author addresses are retained attribution.
- All included image metadata was inspected. EXIF data contains resolution, dimensions and color-space information, with no GPS, personal author or device-identifying fields found. Images are byte-identical to the source snapshot.
- The included video contains a single video stream with technical codec/container metadata. The six captioned scenes were visually reviewed. No personal presentation text was found.
- No PDFs, archives, runtime databases, dependency caches, private keys or environment-secret files are included.

Pattern scanning and content review cannot guarantee the absence of every possible secret. Application tests were not rerun for this packaging-only release. Existing test results remain explicitly historical. The existing Codex test notes record an upstream Next.js installation security warning; dependencies were not upgraded or certified vulnerability-free by this release review.

## Rights review

The source provenance and reuse registers identify the included implementation as original TITAN code. No copied Unreal Engine tree, restricted third-party source, engine binaries or bundled dependency libraries were found. The Unreal bridge consists of TITAN integration source calling external APIs; it does not include Epic's implementation. Qt, Rust and npm dependencies are referenced rather than distributed as library source or binaries.

Existing proprietary/all-rights-reserved notices and contributor attribution are retained. Public visibility is not an open-source license and grants no additional reuse or redistribution rights. This review covers this source snapshot, not a future binary bundle or a separate Unreal Engine distribution.

Relevant primary licensing references: [Unreal Engine EULA](https://www.unrealengine.com/eula/unreal), [Qt licensing obligations](https://www.qt.io/development/open-source-lgpl-obligations). Existing project-specific restrictions remain documented in `TITAN-Studio/docs/LICENSE_AND_RIGHTS_MATRIX.md` and the legacy provenance notes.
