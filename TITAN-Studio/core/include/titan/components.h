// TITAN Engine — component registry: typed, versioned object data with validation and migrations.
// Original TITAN code.
//
// Components live on SceneObject::components (titan/scene.h) and serialize with the scene. The registry knows the
// current version of each type, how to validate it and how to upgrade older data. normalize() is meant for content
// entering the engine: known types are validated and upgraded, unknown types pass through unchanged (forward
// compatibility), and data written by a newer version of a known type is refused rather than guessed.
// Not exposed over the protocol yet: no capability is advertised until it is wired and verified.
#pragma once

#include "titan/scene.h"

#include <functional>
#include <map>
#include <string>

namespace titan {

struct ComponentType {
    std::string type;
    int version = 1;  // current version
    // Throws std::invalid_argument when data at the current version is invalid. Optional.
    std::function<void(const Json& data)> validate;
    // Upgrades data written at `fromVersion` (1 <= fromVersion < version) to the current version; throws
    // std::invalid_argument when it cannot. Required when version > 1.
    std::function<Json(const Json& data, int fromVersion)> migrate;
};

class ComponentRegistry {
public:
    // The engine's built-in types: "titan.tags" v1 ({"tags": [unique strings of 1-64 characters]}).
    static ComponentRegistry withBuiltins();

    // std::invalid_argument for an empty or duplicate type, a version below 1, or a version > 1 without migrate.
    void add(ComponentType t);
    const ComponentType* find(const std::string& type) const;

    // A validated, current-version copy of `c`; unknown types are returned unchanged. Throws
    // std::invalid_argument for invalid data, a failed migration, or a version newer than the registered one.
    Component normalize(const Component& c) const;
    // normalize() for every component in the scene, all or nothing: throws before changing anything.
    void normalizeScene(Scene& scene) const;

private:
    std::map<std::string, ComponentType> types_;
};

}  // namespace titan
