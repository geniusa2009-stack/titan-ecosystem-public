# Licence and Rights Matrix

This records what the **supplied files** establish. It is engineering provenance tracking, not legal
advice. The project owner has stated (in the TITAN directive, 2026-11-05) that they purchased the supplied
source with full ownership and commercial rights. **No agreement document was supplied**, so that statement is
recorded as the owner's claim (O) and is not independently verified (V) here. Engineering work proceeded on
the owner's authorization; the specific items below are flagged because they are written into the source
itself.

| Right | Unreal Engine 5.8.4 archive | TITAN v0.5 | New TITAN 0.6 code | Qt 6.12 (runtime modules used) |
|---|---|---|---|---|
| Source access / internal use | V: present; `LICENSE.md` says use is governed by the Unreal Engine EULA | V | V (authored here) | V (LGPL-3.0) |
| Modification / derivative works | Owner claim (O); the EULA also permits licensee modification | V (TITAN-authored) | V | V (LGPL obligations) |
| Commercial use | O | O | O (owner's product) | V (dynamic linking, LGPL) |
| Binary redistribution | O; EULA terms and royalty provisions apply unless a custom agreement says otherwise | O | O | V (ship LGPL notices; allow relinking) |
| Source redistribution | **Restricted by the EULA unless a custom agreement says otherwise.** Do not publish. | O | O | V |
| Rebranding / trademarks | "Unreal" trademarks are not included in a source licence; TITAN branding avoids them | — | V (original TITAN identity) | Qt trademarks not used |
| Sublicensing to TITAN customers | **Unknown: requires the actual agreement** | O | O | n/a |
| External AI processing / disclosure | Unknown. UE source processed **locally** only; it has never been uploaded or pushed. | — | — | — |
| Embedded third-party components | 862 `.tps` notices; each has its own terms (see THIRD_PARTY_DEPENDENCIES.md) | none | none | Qt's bundled 3rd-party (zlib, harfbuzz, ICU…) per Qt docs |

## Concrete restrictions identified (raised once, as required)

1. **Engine/source redistribution:** `UnrealEngine-release/LICENSE.md` places the code under the Unreal
   Engine EULA. Distributing TITAN Studio + an engine built from this source to customers, or exposing
   the source, depends on the custom agreement the owner holds. Affected component: everything under
   `vault/extracted/ue-5.8.4` and any future build of it. *Engineering impact today: none.* The UE tree is
   kept private, read-only and outside the TITAN git repository.
2. **Qt licensing choice:** Qt Quick 3D and Qt Shader Tools are available under GPL-3.0 or a commercial
   licence, **not LGPL**. TITAN Studio therefore does **not** use Qt Quick 3D. Its viewport is original
   code on QRhi (part of Qt GUI, LGPL-3.0). Qt Shader Tools runs **only at build time** (`qsb` bakes
   shaders); TITAN Studio does not link it. A commercial Qt licence would remove these constraints.
3. **QRhi is semi-public Qt API** (requires `Qt6::GuiPrivate`): no binary-compatibility guarantee across Qt
   minor versions. This is a maintenance risk, not a licence issue.

## Rules applied in this repository

- No UE source is copied into the TITAN git repository. `unreal/Plugins/TitanStudioBridge` is original
  code that *calls* UE APIs.
- No partner source exists yet. The Source Vault workflow (SOURCE_INTEGRATION_DECISIONS.md) applies when it
  arrives.
- The TITAN repository (TITAN-owned code only) is backed up to the **private** GitHub repository
  `geniusa2009-stack/TITAN` at the owner's direction (2026-11-06). The Unreal Engine source fork
  (`~/TitanEngine/UE-5.8.4`) and the supplied archives under `vault/` are not in this repository and
  have not been pushed anywhere.
