// TITAN Engine — Edit/Play state model (see titan/play_session.h). Original TITAN code.
#include "titan/play_session.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace titan {

void PlaySession::begin(const Scene& scene) {
    if (saved_) throw std::logic_error("already in play mode");
    saved_ = scene;
}

Scene PlaySession::end(const Scene& playScene) {
    if (!saved_) throw std::logic_error("not in play mode");
    Scene restored = std::move(*saved_);
    saved_.reset();
    restored.setRevision(std::max(restored.revision(), playScene.revision()) + 1);
    return restored;
}

}  // namespace titan
