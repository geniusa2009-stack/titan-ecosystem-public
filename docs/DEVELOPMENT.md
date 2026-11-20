# Development and collaboration

TITAN explores a simple idea: learning, writing and building software belong in a connected workflow. Integrated software environments are a design inspiration. TITAN is an independent project.

## Working across three layers

**Code** handles the details beneath editing: text coordinates, document versions, disk writes and recovery. The Rust crates separate pure text processing from I/O and protocol handling, so changes to one layer can be tested without involving an entire editor.

**Studio** makes the process boundary visible. The engine host owns scene state. The desktop editor sends commands, receives updates and renders the reference scene with Metal. Capability negotiation keeps the UI aligned with the selected backend.

**Codex** works at the learning layer. React state drives answer selection and feedback. Its interface explores how a student moves from an explanation to a small programming question. Connecting these exercises to real development workspaces is a goal for the ecosystem.

## Engineering lessons

The Code recovery tests exercise interrupted writes and process crashes. They make persistence a behavior that can be tested, rather than an assumption. Revision checks address another practical problem: an edit must apply to the version it was calculated against.

Studio's command protocol keeps scene ownership explicit. Undo, save/reopen and hierarchy operations need to preserve that agreement between the UI and the host. Its backend capability contract also gives unsupported operations a predictable response.

Performance work needs repeatable workloads. Code includes benchmark scenarios and resource-aware background work. Historical measurements and raw reports are retained alongside the source so that future changes can be compared under the same conditions.

## Collaboration

The projects are collaborative. Their architecture spans Rust systems work, C++/QML desktop development and TypeScript interfaces. The project is presented as team work; source references and attribution remain in SOURCES.md. Historical co-author attribution is documented there; historical commits are not imported.

The direction for the ecosystem is to make these layers fit together: a programming lesson can become a workspace, and a workspace can lead to something the student builds and explores.
