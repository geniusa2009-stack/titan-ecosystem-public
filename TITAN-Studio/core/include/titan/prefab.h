// TITAN Engine — prefabs: reusable object hierarchies stored as assets (<project>/Assets/**/*.tprefab).
// Original TITAN code.
//
// A prefab holds a copy of one object and its descendants with prefab-local ids (1..n, root first, root
// parent 0). Instantiating allocates fresh scene ids, keeps the hierarchy, places the root and links every
// new object to its prefab (SceneObject::prefab). File format: {"format":1,"id":"<32 hex>","name":...,
// "objects":[SceneObject JSON with local ids]}.
#pragma once

#include "titan/scene.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace titan {

struct Prefab {
    static constexpr int kFormatVersion = 1;
    static constexpr size_t kMaxObjects = 10000;
    std::string id;    // stable prefab id (32 lowercase hex)
    std::string name;
    std::vector<SceneObject> objects;  // local ids, objects[0] is the root (parent 0), parents before children

    Json toJson() const;
    static Prefab fromJson(const Json& j);  // validates format, id, ids, parents (no cycles, one root)

    // Copies `root` and its descendants out of `scene`. The root keeps its own transform; descendants keep
    // their local transforms. Generator provenance is not copied (a prefab is plain content).
    static Prefab fromSubtree(const Scene& scene, ObjectId root, const std::string& name, const std::string& id);

    // New scene objects (fresh ids from `scene.allocateId()`, not inserted) in parents-first order, linked to
    // this prefab. The root is parented to `parent` and moved to `position` when given.
    std::vector<SceneObject> instantiate(Scene& scene, ObjectId parent = kNoObject,
                                         const std::optional<Vec3>& position = std::nullopt) const;
};

Prefab readPrefabFile(const std::filesystem::path& file);  // throws on I/O or validation errors
std::string newPrefabId();                                 // 32 random lowercase hex characters

}  // namespace titan
