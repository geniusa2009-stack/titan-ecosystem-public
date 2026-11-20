# Architecture Decision Records

Source-related decisions D-1 to D-6 are in `SOURCE_INTEGRATION_DECISIONS.md`.

**ADR-1: Engine owns canonical state; editors are views.** Every mutation goes through `object.*` or
`worldbuilder.*` requests and becomes a `SceneEdit` on the engine's undo stack. Studio's `SceneModel` is
refreshed only from snapshots and events. *Consequence:* multiple editors stay consistent (tested with two
clients), and the Slate bridge and AI tools reuse the same undo and validation.

**ADR-2: Snapshot-based reversible edits** (before/after object snapshots) instead of per-property
command classes. This keeps one generic, testable mechanism for create, delete, update and generation.
Cost: memory proportional to the objects touched, which is acceptable at editor scale.

**ADR-3: JSON over a length-prefixed local socket.** It is debuggable, versioned via `session.hello`, and
language-neutral (Qt, UE and Python clients). Binary payloads such as meshes and frames will use shared
memory or GPU handles, not JSON.

**ADR-4: C ABI + JSON for native extensions.** This avoids C++ ABI and STL coupling across compilers.
Manifests are validated before `dlopen`. Commands must be declared in the manifest. Extensions *propose*
and the engine *commits*.

**ADR-5: Deterministic generation with an owned PRNG (PCG32).** It avoids `std::` distributions, whose
output varies by standard library. Recipes are stored in the scene for non-destructive regeneration.

**ADR-6: Studio viewport on QRhi rather than Qt Quick 3D.** This keeps the runtime LGPL-compatible and
uses the same RHI that Qt Quick itself uses (Metal on macOS). It can later composite engine-produced
textures.

**ADR-7: Engineering Agent (specified, not built).** Workflow: Understand → Plan → `git worktree add` on
`agent/<task>` → modify → build/test via CTest → present diff + results → explicit approval → merge. The
default is read-only, with tools allow-listed per task and every command audit-logged. Restricted source
is never sent to external model providers unless the owner's agreement permits it.
