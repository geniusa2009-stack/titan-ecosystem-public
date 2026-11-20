// TITAN Engine — object clipboard: a self-contained, versioned copy of scene objects. Original TITAN code.
//
// Format: {"format": "titan.clipboard", "version": 1, "sourceProject": "...", "objects": [SceneObject JSON]}
// with clipboard-local ids 1..n in parents-first order. Roots (parent 0) carry their world transform, so a
// paste lands where the copy was taken from, at the scene root or under any parent. Pasting allocates fresh
// scene ids and remaps every parent link, so no pasted object can point at an object outside the paste.
// World Builder provenance is never copied; prefab links survive only when pasting into the same project.
#pragma once

#include "titan/scene.h"

#include <string>
#include <vector>

namespace titan {

struct Clipboard {
    static constexpr const char* kFormat = "titan.clipboard";
    static constexpr int kVersion = 1;
    static constexpr size_t kMaxObjects = 10000;
    std::string sourceProject;
    std::vector<SceneObject> objects;

    // Copies `ids` (an id whose ancestor is also listed comes along with it, not twice) and their descendants.
    // Throws std::invalid_argument for unknown ids, an empty list, or a root whose world transform has no
    // position/rotation/scale form (sheared by a non-uniformly scaled, rotated ancestor chain).
    static Clipboard copy(const Scene& scene, const std::vector<ObjectId>& ids, const std::string& sourceProject);
    Json toJson() const;
    static Clipboard fromJson(const Json& j);  // validates format, version, ids, parents (forward links only)

    // Fresh objects for `scene` (ids allocated, not inserted), parents first. Roots go under `parent` with their
    // world transform expressed in its space. Throws std::invalid_argument for an unknown parent or when a root's
    // placement cannot be expressed under it (shear); every check runs before any id is allocated, so a refused
    // paste leaves `scene` untouched. `sameProject` keeps prefab links.
    std::vector<SceneObject> paste(Scene& scene, ObjectId parent, bool sameProject) const;
};

}  // namespace titan
