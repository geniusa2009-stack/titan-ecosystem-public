// TITAN Engine — canonical scene model. The engine process owns the only authoritative
// instance; editors receive serialized snapshots and stable ObjectIds, never pointers.
// Original TITAN code.
#pragma once

#include "titan/json.h"
#include "titan/math.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace titan {

using ObjectId = uint64_t;            // 0 is "none"; ids are never reused within a scene
constexpr ObjectId kNoObject = 0;
constexpr ObjectId kMaxObjectId = (1ull << 53) - 1;  // exact in JSON / JavaScript numbers

enum class ObjectType { Group, Mesh, Light, Camera };
enum class Shape { None, Cube, Plane, Sphere, Cylinder };

const char* toString(ObjectType t);
const char* toString(Shape s);
std::optional<ObjectType> objectTypeFromString(const std::string& s);
std::optional<Shape> shapeFromString(const std::string& s);

struct Color {
    float r = 0.8f, g = 0.8f, b = 0.8f;
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Color& o) const { return !(*this == o); }
};

// Provenance for procedurally generated content (non-destructive World Builder output).
struct GeneratorTag {
    std::string extension;  // e.g. "titan.procedural.scatter"
    std::string recipeId;   // id of the recipe stored in Scene::recipes
    bool operator==(const GeneratorTag& o) const { return extension == o.extension && recipeId == o.recipeId; }
};

// Link from an object to the prefab it was instantiated from (see titan/prefab.h).
struct PrefabLink {
    std::string prefabId;  // 32 hex characters, the prefab's stable id
    ObjectId localId = 0;  // id of the object inside the prefab
    bool operator==(const PrefabLink& o) const { return prefabId == o.prefabId && localId == o.localId; }
};

// A typed, versioned block of data attached to an object (registry, validation, migrations:
// titan/components.h). Types this engine does not know are kept verbatim, so newer files survive a round trip.
struct Component {
    std::string type;  // namespaced, e.g. "titan.tags"
    int version = 1;
    Json data = Json::object();
    bool operator==(const Component& o) const { return type == o.type && version == o.version && data == o.data; }
};

struct SceneObject {
    ObjectId id = kNoObject;
    std::string name;
    ObjectType type = ObjectType::Mesh;
    Shape shape = Shape::Cube;
    ObjectId parent = kNoObject;
    Transform transform;
    Color color;
    bool visible = true;
    bool locked = false;  // editor lock: not pickable, transform edits refused; serialized only when true
    float intensity = 1.0f;  // lights only
    std::optional<GeneratorTag> generator;
    std::optional<PrefabLink> prefab;
    std::vector<Component> components;  // at most one per type; serialized only when non-empty

    Json toJson() const;
    static SceneObject fromJson(const Json& j);  // throws JsonError / std::invalid_argument
    bool operator==(const SceneObject& o) const;
    bool operator!=(const SceneObject& o) const { return !(*this == o); }
};

struct Recipe {
    std::string id;
    std::string extension;
    Json params;
    Json toJson() const;
    static Recipe fromJson(const Json& j);
};

struct Environment {
    Color sky{0.62f, 0.72f, 0.86f};
    float ambient = 0.28f;
    Json toJson() const;
    static Environment fromJson(const Json& j);
};

class Scene {
public:
    static constexpr int kFormatVersion = 1;

    std::string name = "Untitled";
    Environment environment;
    std::vector<Recipe> recipes;

    // Object access (ordered by id for deterministic iteration and serialization).
    const std::map<ObjectId, SceneObject>& objects() const { return objects_; }
    const SceneObject* find(ObjectId id) const;
    bool contains(ObjectId id) const { return objects_.count(id) != 0; }
    size_t size() const { return objects_.size(); }

    ObjectId allocateId();
    ObjectId peekNextId() const { return nextId_; }

    // Low-level mutation used by SceneEdit; validates parent links.
    void put(const SceneObject& obj);
    void erase(ObjectId id);

    std::vector<ObjectId> childrenOf(ObjectId id) const;
    std::vector<ObjectId> withDescendants(const std::vector<ObjectId>& ids) const;  // parents first
    Mat4 worldMatrix(ObjectId id) const;

    const Recipe* findRecipe(const std::string& id) const;
    void putRecipe(const Recipe& r);
    void eraseRecipe(const std::string& id);

    uint64_t revision() const { return revision_; }
    void bumpRevision() { ++revision_; }
    void setRevision(uint64_t r) { revision_ = r; }

    Json toJson() const;
    static Scene fromJson(const Json& j);  // validates structure, ids and parent links

    static Scene makeDefault(const std::string& name);

private:
    std::map<ObjectId, SceneObject> objects_;
    ObjectId nextId_ = 1;
    uint64_t revision_ = 0;
};

// Ray picking against the visible objects' exact primitive shapes (in world space).
struct PickResult {
    ObjectId id = kNoObject;
    float distance = 0;
};
std::optional<float> intersectShape(Shape shape, const Ray& localRay);
std::optional<PickResult> pick(const Scene& scene, const Ray& worldRay);

Json toJson(const Transform& t);
Transform transformFromJson(const Json& j, const Transform& base = {});
Json toJson(const Vec3& v);
Vec3 vec3FromJson(const Json& j);
Json toJson(const Color& c);
Color colorFromJson(const Json& j);

}  // namespace titan
