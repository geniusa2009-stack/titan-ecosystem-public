// TITAN Engine — scene model, serialization and picking. Original TITAN code.
#include "titan/scene.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace titan {

const char* toString(ObjectType t) {
    switch (t) {
        case ObjectType::Group: return "group";
        case ObjectType::Mesh: return "mesh";
        case ObjectType::Light: return "light";
        case ObjectType::Camera: return "camera";
    }
    return "mesh";
}

const char* toString(Shape s) {
    switch (s) {
        case Shape::None: return "none";
        case Shape::Cube: return "cube";
        case Shape::Plane: return "plane";
        case Shape::Sphere: return "sphere";
        case Shape::Cylinder: return "cylinder";
    }
    return "none";
}

std::optional<ObjectType> objectTypeFromString(const std::string& s) {
    if (s == "group") return ObjectType::Group;
    if (s == "mesh") return ObjectType::Mesh;
    if (s == "light") return ObjectType::Light;
    if (s == "camera") return ObjectType::Camera;
    return std::nullopt;
}

std::optional<Shape> shapeFromString(const std::string& s) {
    if (s == "none") return Shape::None;
    if (s == "cube") return Shape::Cube;
    if (s == "plane") return Shape::Plane;
    if (s == "sphere") return Shape::Sphere;
    if (s == "cylinder") return Shape::Cylinder;
    return std::nullopt;
}

Json toJson(const Vec3& v) { return Json(Json::Array{Json(v.x), Json(v.y), Json(v.z)}); }

Vec3 vec3FromJson(const Json& j) {
    const auto& a = j.asArray();
    if (a.size() != 3) throw JsonError("expected [x, y, z]");
    Vec3 v{static_cast<float>(a[0].asNumber()), static_cast<float>(a[1].asNumber()),
           static_cast<float>(a[2].asNumber())};
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) throw JsonError("non-finite vector");
    return v;
}

Json toJson(const Color& c) { return Json(Json::Array{Json(c.r), Json(c.g), Json(c.b)}); }

Color colorFromJson(const Json& j) {
    Vec3 v = vec3FromJson(j);
    auto clamp01 = [](float f) { return std::min(1.0f, std::max(0.0f, f)); };
    return {clamp01(v.x), clamp01(v.y), clamp01(v.z)};
}

Json toJson(const Transform& t) {
    Json j = Json::object();
    j["position"] = toJson(t.position);
    j["rotation"] = toJson(t.rotation);
    j["scale"] = toJson(t.scale);
    return j;
}

Transform transformFromJson(const Json& j, const Transform& base) {
    Transform t = base;
    if (!j.isObject()) throw JsonError("transform must be an object");
    if (const Json* p = j.find("position")) t.position = vec3FromJson(*p);
    if (const Json* r = j.find("rotation")) t.rotation = vec3FromJson(*r);
    if (const Json* s = j.find("scale")) t.scale = vec3FromJson(*s);
    return t;
}

Json SceneObject::toJson() const {
    Json j = Json::object();
    j["id"] = id;
    j["name"] = name;
    j["type"] = toString(type);
    j["shape"] = toString(shape);
    j["parent"] = parent;
    j["transform"] = titan::toJson(transform);
    j["color"] = titan::toJson(color);
    j["visible"] = visible;
    if (locked) j["locked"] = true;  // absent when false: unlocked scenes serialize exactly as before
    if (type == ObjectType::Light) j["intensity"] = intensity;
    if (generator) {
        Json g = Json::object();
        g["extension"] = generator->extension;
        g["recipeId"] = generator->recipeId;
        j["generator"] = g;
    }
    if (prefab) {
        Json p = Json::object();
        p["id"] = prefab->prefabId;
        p["local"] = prefab->localId;
        j["prefab"] = p;
    }
    if (!components.empty()) {  // absent when empty: scenes without components serialize exactly as before
        Json cs = Json::array();
        for (const auto& c : components) {
            Json cj = Json::object();
            cj["type"] = c.type;
            cj["version"] = c.version;
            cj["data"] = c.data;
            cs.push_back(cj);
        }
        j["components"] = cs;
    }
    return j;
}

SceneObject SceneObject::fromJson(const Json& j) {
    if (!j.isObject()) throw JsonError("object entry must be an object");
    SceneObject o;
    o.id = j["id"].asUInt();
    if (o.id == kNoObject || o.id > kMaxObjectId) throw JsonError("object id out of range");
    o.name = j.getString("name", "Object");
    auto type = objectTypeFromString(j.getString("type", "mesh"));
    if (!type) throw JsonError("unknown object type");
    o.type = *type;
    auto shape = shapeFromString(j.getString("shape", o.type == ObjectType::Group ? "none" : "cube"));
    if (!shape) throw JsonError("unknown shape");
    o.shape = *shape;
    o.parent = j.contains("parent") ? j["parent"].asUInt() : kNoObject;
    if (const Json* t = j.find("transform")) o.transform = transformFromJson(*t);
    if (const Json* c = j.find("color")) o.color = colorFromJson(*c);
    o.visible = j.getBool("visible", true);
    o.locked = j.getBool("locked", false);
    o.intensity = static_cast<float>(j.getNumber("intensity", 1.0));
    if (const Json* g = j.find("generator"); g && g->isObject())
        o.generator = GeneratorTag{g->getString("extension"), g->getString("recipeId")};
    if (const Json* p = j.find("prefab"); p && p->isObject())
        o.prefab = PrefabLink{p->getString("id"), ObjectId(p->getNumber("local", 0))};
    if (const Json* cs = j.find("components")) {
        if (!cs->isArray()) throw JsonError("components must be an array");
        for (const auto& cj : cs->asArray()) {
            if (!cj.isObject()) throw JsonError("component must be an object");
            Component c;
            c.type = cj.getString("type");
            if (c.type.empty() || c.type.size() > 128) throw JsonError("component type must be 1-128 characters");
            const double v = cj.getNumber("version", 1);
            if (!(v >= 1 && v <= 1e6) || v != std::floor(v)) throw JsonError("component version must be a positive integer");
            c.version = int(v);
            if (const Json* d = cj.find("data")) {
                if (!d->isObject()) throw JsonError("component data must be an object");
                c.data = *d;
            }
            for (const auto& e : o.components)
                if (e.type == c.type) throw JsonError("duplicate component '" + c.type + "'");
            o.components.push_back(std::move(c));
        }
    }
    return o;
}

bool SceneObject::operator==(const SceneObject& o) const {
    return id == o.id && name == o.name && type == o.type && shape == o.shape && parent == o.parent &&
           transform == o.transform && color == o.color && visible == o.visible && locked == o.locked && intensity == o.intensity &&
           generator == o.generator && prefab == o.prefab && components == o.components;
}

Json Recipe::toJson() const {
    Json j = Json::object();
    j["id"] = id;
    j["extension"] = extension;
    j["params"] = params;
    return j;
}

Recipe Recipe::fromJson(const Json& j) {
    Recipe r;
    r.id = j["id"].asString();
    r.extension = j["extension"].asString();
    r.params = j["params"];
    return r;
}

Json Environment::toJson() const {
    Json j = Json::object();
    j["sky"] = titan::toJson(sky);
    j["ambient"] = ambient;
    return j;
}

Environment Environment::fromJson(const Json& j) {
    Environment e;
    if (!j.isObject()) return e;
    if (const Json* s = j.find("sky")) e.sky = colorFromJson(*s);
    e.ambient = static_cast<float>(j.getNumber("ambient", e.ambient));
    return e;
}

const SceneObject* Scene::find(ObjectId id) const {
    auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : &it->second;
}

ObjectId Scene::allocateId() {
    if (nextId_ > kMaxObjectId) throw std::overflow_error("scene object id space exhausted");
    return nextId_++;
}

void Scene::put(const SceneObject& obj) {
    if (obj.id == kNoObject || obj.id > kMaxObjectId) throw std::invalid_argument("invalid object id");
    if (obj.parent == obj.id) throw std::invalid_argument("object cannot parent itself");
    // Reject cycles: walk the prospective parent chain.
    for (ObjectId p = obj.parent; p != kNoObject;) {
        if (p == obj.id) throw std::invalid_argument("parent cycle");
        const SceneObject* po = find(p);
        if (!po) break;  // missing parents are tolerated transiently during batch edits
        p = po->parent;
    }
    objects_[obj.id] = obj;
    if (obj.id >= nextId_) nextId_ = obj.id + 1;
}

void Scene::erase(ObjectId id) { objects_.erase(id); }

std::vector<ObjectId> Scene::childrenOf(ObjectId id) const {
    std::vector<ObjectId> out;
    for (const auto& [oid, o] : objects_)
        if (o.parent == id) out.push_back(oid);
    return out;
}

std::vector<ObjectId> Scene::withDescendants(const std::vector<ObjectId>& ids) const {
    std::vector<ObjectId> out;
    std::set<ObjectId> seen;
    std::vector<ObjectId> queue(ids.begin(), ids.end());
    for (size_t i = 0; i < queue.size(); ++i) {
        ObjectId id = queue[i];
        if (!contains(id) || !seen.insert(id).second) continue;
        out.push_back(id);
        for (ObjectId c : childrenOf(id)) queue.push_back(c);
    }
    return out;
}

Mat4 Scene::worldMatrix(ObjectId id) const {
    Mat4 m;
    int guard = 0;
    for (const SceneObject* o = find(id); o && guard < 1024; o = find(o->parent), ++guard)
        m = o->transform.matrix() * m;
    return m;
}

const Recipe* Scene::findRecipe(const std::string& id) const {
    for (const auto& r : recipes)
        if (r.id == id) return &r;
    return nullptr;
}

void Scene::putRecipe(const Recipe& r) {
    for (auto& existing : recipes)
        if (existing.id == r.id) {
            existing = r;
            return;
        }
    recipes.push_back(r);
}

void Scene::eraseRecipe(const std::string& id) {
    recipes.erase(std::remove_if(recipes.begin(), recipes.end(), [&](const Recipe& r) { return r.id == id; }),
                  recipes.end());
}

Json Scene::toJson() const {
    Json j = Json::object();
    j["format"] = "titan.scene";
    j["formatVersion"] = kFormatVersion;
    j["name"] = name;
    j["nextId"] = nextId_;
    j["environment"] = environment.toJson();
    Json objs = Json::array();
    for (const auto& [id, o] : objects_) objs.push_back(o.toJson());
    j["objects"] = objs;
    Json rs = Json::array();
    for (const auto& r : recipes) rs.push_back(r.toJson());
    j["recipes"] = rs;
    return j;
}

Scene Scene::fromJson(const Json& j) {
    if (j.getString("format") != "titan.scene") throw JsonError("not a TITAN scene file");
    int version = static_cast<int>(j["formatVersion"].asInt());
    if (version < 1 || version > kFormatVersion)
        throw JsonError("unsupported scene formatVersion " + std::to_string(version));
    Scene s;
    s.name = j.getString("name", "Untitled");
    s.environment = Environment::fromJson(j["environment"]);
    for (const Json& oj : j["objects"].asArray()) {
        SceneObject o = SceneObject::fromJson(oj);
        if (s.contains(o.id)) throw JsonError("duplicate object id " + std::to_string(o.id));
        s.objects_[o.id] = o;
    }
    for (const auto& [id, o] : s.objects_)
        if (o.parent != kNoObject && !s.contains(o.parent))
            throw JsonError("object " + std::to_string(id) + " references missing parent");
    for (const auto& [id, o] : s.objects_) {
        int guard = 0;
        for (ObjectId p = o.parent; p != kNoObject; p = s.objects_.at(p).parent)
            if (p == id || ++guard > 1024) throw JsonError("parent cycle at object " + std::to_string(id));
    }
    ObjectId maxId = s.objects_.empty() ? 0 : s.objects_.rbegin()->first;
    s.nextId_ = std::max<ObjectId>(maxId + 1, j.contains("nextId") ? j["nextId"].asUInt() : 1);
    if (const Json* rs = j.find("recipes"); rs && rs->isArray())
        for (const Json& rj : rs->asArray()) s.recipes.push_back(Recipe::fromJson(rj));
    return s;
}

Scene Scene::makeDefault(const std::string& name) {
    Scene s;
    s.name = name;
    auto add = [&](const std::string& n, ObjectType type, Shape shape, Vec3 pos, Vec3 rot, Vec3 scale, Color c,
                   ObjectId parent = kNoObject) {
        SceneObject o;
        o.id = s.allocateId();
        o.name = n;
        o.type = type;
        o.shape = shape;
        o.parent = parent;
        o.transform = {pos, rot, scale};
        o.color = c;
        s.objects_[o.id] = o;
        return o.id;
    };
    add("Ground", ObjectType::Mesh, Shape::Plane, {0, 0, 0}, {0, 0, 0}, {24, 1, 24}, {0.42f, 0.45f, 0.48f});
    ObjectId sun = add("Sun", ObjectType::Light, Shape::Sphere, {6, 9, 4}, {-50, 35, 0}, {0.5f, 0.5f, 0.5f},
                       {1.0f, 0.95f, 0.85f});
    s.objects_[sun].intensity = 1.0f;
    add("Main Camera", ObjectType::Camera, Shape::Cube, {0, 3, 12}, {-12, 0, 0}, {0.4f, 0.3f, 0.6f},
        {0.18f, 0.2f, 0.24f});
    add("Hero Cube", ObjectType::Mesh, Shape::Cube, {0, 1, 0}, {0, 20, 0}, {2, 2, 2}, {0.44f, 0.6f, 0.84f});
    add("Sphere", ObjectType::Mesh, Shape::Sphere, {-3.5f, 0.75f, 1.5f}, {0, 0, 0}, {1.5f, 1.5f, 1.5f},
        {0.86f, 0.52f, 0.38f});
    add("Pillar", ObjectType::Mesh, Shape::Cylinder, {3.5f, 1.5f, -1}, {0, 0, 0}, {1, 3, 1},
        {0.78f, 0.78f, 0.74f});
    ObjectId props = add("Props", ObjectType::Group, Shape::None, {0, 0, -4}, {0, 0, 0}, {1, 1, 1},
                         {0.8f, 0.8f, 0.8f});
    add("Crate A", ObjectType::Mesh, Shape::Cube, {-1.2f, 0.5f, 0}, {0, 10, 0}, {1, 1, 1}, {0.62f, 0.5f, 0.36f},
        props);
    add("Crate B", ObjectType::Mesh, Shape::Cube, {0.4f, 0.5f, 0.3f}, {0, -25, 0}, {1, 1, 1},
        {0.58f, 0.47f, 0.33f}, props);
    add("Crate C", ObjectType::Mesh, Shape::Cube, {-0.4f, 1.5f, 0.1f}, {0, 40, 0}, {1, 1, 1},
        {0.66f, 0.54f, 0.4f}, props);
    return s;
}

// ---- picking -------------------------------------------------------------------------

std::optional<float> intersectShape(Shape shape, const Ray& r) {
    const float eps = 1e-8f;
    auto best = [](std::optional<float> a, std::optional<float> b) -> std::optional<float> {
        if (!a) return b;
        if (!b) return a;
        return std::min(*a, *b);
    };
    switch (shape) {
        case Shape::None: return std::nullopt;
        case Shape::Cube: {  // slab test against [-0.5, 0.5]^3
            float tmin = -1e30f, tmax = 1e30f;
            const float o[3] = {r.origin.x, r.origin.y, r.origin.z};
            const float d[3] = {r.direction.x, r.direction.y, r.direction.z};
            for (int i = 0; i < 3; ++i) {
                if (std::fabs(d[i]) < eps) {
                    if (o[i] < -0.5f || o[i] > 0.5f) return std::nullopt;
                    continue;
                }
                float t1 = (-0.5f - o[i]) / d[i], t2 = (0.5f - o[i]) / d[i];
                if (t1 > t2) std::swap(t1, t2);
                tmin = std::max(tmin, t1);
                tmax = std::min(tmax, t2);
                if (tmin > tmax) return std::nullopt;
            }
            if (tmax < 0) return std::nullopt;
            return tmin >= 0 ? tmin : tmax;
        }
        case Shape::Plane: {  // y = 0, |x|,|z| <= 0.5, double sided
            if (std::fabs(r.direction.y) < eps) return std::nullopt;
            float t = -r.origin.y / r.direction.y;
            if (t < 0) return std::nullopt;
            Vec3 p = r.origin + r.direction * t;
            if (std::fabs(p.x) > 0.5f || std::fabs(p.z) > 0.5f) return std::nullopt;
            return t;
        }
        case Shape::Sphere: {  // radius 0.5 at origin
            float a = dot(r.direction, r.direction);
            float b = 2 * dot(r.origin, r.direction);
            float c = dot(r.origin, r.origin) - 0.25f;
            float disc = b * b - 4 * a * c;
            if (disc < 0 || a < eps) return std::nullopt;
            float sq = std::sqrt(disc);
            float t0 = (-b - sq) / (2 * a), t1 = (-b + sq) / (2 * a);
            if (t0 >= 0) return t0;
            if (t1 >= 0) return t1;
            return std::nullopt;
        }
        case Shape::Cylinder: {  // radius 0.5 around Y, y in [-0.5, 0.5], capped
            std::optional<float> hit;
            float a = r.direction.x * r.direction.x + r.direction.z * r.direction.z;
            if (a > eps) {
                float b = 2 * (r.origin.x * r.direction.x + r.origin.z * r.direction.z);
                float c = r.origin.x * r.origin.x + r.origin.z * r.origin.z - 0.25f;
                float disc = b * b - 4 * a * c;
                if (disc >= 0) {
                    float sq = std::sqrt(disc);
                    for (float t : {(-b - sq) / (2 * a), (-b + sq) / (2 * a)}) {
                        float y = r.origin.y + r.direction.y * t;
                        if (t >= 0 && y >= -0.5f && y <= 0.5f) hit = best(hit, t);
                    }
                }
            }
            if (std::fabs(r.direction.y) > eps) {
                for (float capY : {-0.5f, 0.5f}) {
                    float t = (capY - r.origin.y) / r.direction.y;
                    Vec3 p = r.origin + r.direction * t;
                    if (t >= 0 && p.x * p.x + p.z * p.z <= 0.25f) hit = best(hit, t);
                }
            }
            return hit;
        }
    }
    return std::nullopt;
}

std::optional<PickResult> pick(const Scene& scene, const Ray& worldRay) {
    std::optional<PickResult> best;
    for (const auto& [id, o] : scene.objects()) {
        if (!o.visible || o.locked || o.shape == Shape::None) continue;
        Mat4 world = scene.worldMatrix(id);
        auto inv = world.inverse();
        if (!inv) continue;
        // Transforming the direction without normalizing keeps the ray parameter t
        // identical in local and world space, so distances compare across objects.
        Ray local{inv->transformPoint(worldRay.origin), inv->transformDirection(worldRay.direction)};
        auto t = intersectShape(o.shape, local);
        if (!t) continue;
        float dist = *t * length(worldRay.direction);
        if (!best || dist < best->distance) best = PickResult{id, dist};
    }
    return best;
}

}  // namespace titan
