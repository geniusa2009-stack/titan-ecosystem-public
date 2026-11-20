# Source Provenance

| Path in repository | Origin | Author | Modified? |
|---|---|---|---|
| `legacy/TitanFoundation-v0.5/**` | TITAN v0.5 archive (sha256 `0ec27747…`) | Prior TITAN work | **No**: imported verbatim (commit `408fa2a`, branch `titan/baseline-v0.5`); only `.github/` was dropped |
| `core/**`, `engine/**`, `sdk/**`, `extensions/**`, `tools/**`, `tests/**` | New in 0.6 | Original TITAN code (this session) | — |
| `studio/**` (C++, QML, GLSL shaders) | New in 0.6 | Original TITAN code and design | — |
| `unreal/Plugins/TitanStudioBridge/**` | New in 0.6 | Original TITAN code that **calls** UE 5.8.4 APIs; contains no Epic source | — |
| `docs/**` | New in 0.6 | TITAN | — |

- No code was copied from the Unreal Engine tree. UE files were read to verify API names and signatures
  (`Ticker.h`, `Actor.h`, `EditorEngine.h`, `ScopedTransaction.h`, `FileHelpers.h`, `CityHash.h`).
- The TITAN mark (rounded square with a geometric "T"), the colour tokens and the UI layout are original.
  The UI uses no Apple artwork, SF Symbols, icon fonts or font files. Icons are Unicode geometric
  characters rendered with system fonts.
- Algorithms used are standard and textbook: slab ray–box test, analytic ray–sphere/cylinder,
  cofactor 4×4 inverse, PCG32 (O'Neill, 2014, an algorithm description reimplemented here), and
  hemispheric ambient lighting.
