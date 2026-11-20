// TITAN Engine — Edit/Play state model. Original TITAN code.
//
// begin() keeps an exact copy of the edit-time scene; end() hands it back. Nothing done while playing (objects
// changed, created or deleted, ids allocated, recipes, environment, components) can reach the edit scene, its
// undo history or its saved file. The restored scene's revision is moved past the play scene's, so editors that
// track revisions resynchronize. Not wired to the protocol yet: no "play" capability is advertised until the
// engine integration (refusing save/undo while playing, autosave pause) is implemented and verified.
#pragma once

#include "titan/scene.h"

#include <optional>

namespace titan {

enum class EditorMode { Edit, Play };

class PlaySession {
public:
    EditorMode mode() const { return saved_ ? EditorMode::Play : EditorMode::Edit; }
    bool playing() const { return saved_.has_value(); }
    // Enters play mode with a copy of `scene`; the caller then simulates on its own scene. std::logic_error if
    // already playing.
    void begin(const Scene& scene);
    // Leaves play mode: returns the edit-time scene with a revision above `playScene`'s. std::logic_error if not
    // playing.
    Scene end(const Scene& playScene);
    // The saved edit-time scene while playing, else nullptr.
    const Scene* editScene() const { return saved_ ? &*saved_ : nullptr; }

private:
    std::optional<Scene> saved_;
};

}  // namespace titan
