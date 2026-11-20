// TITAN Engine — object clipboard (see titan/clipboard.h). Original TITAN code.
#include "titan/clipboard.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace titan {

Clipboard Clipboard::copy(const Scene& scene, const std::vector<ObjectId>& ids, const std::string& sourceProject) {
    std::set<ObjectId> listed;
    for (ObjectId id : ids) {
        if (!scene.contains(id)) throw std::invalid_argument("object " + std::to_string(id) + " not found");
        listed.insert(id);
    }
    std::vector<ObjectId> roots;
    for (ObjectId id : ids) {
        bool nested = false;
        for (ObjectId p = scene.find(id)->parent; p != kNoObject && !nested; p = scene.find(p)->parent) nested = listed.count(p) != 0;
        if (!nested && std::find(roots.begin(), roots.end(), id) == roots.end()) roots.push_back(id);
    }
    if (roots.empty()) throw std::invalid_argument("nothing to copy");
    Clipboard c;
    c.sourceProject = sourceProject;
    std::map<ObjectId, ObjectId> local;
    for (ObjectId id : scene.withDescendants(roots)) {  // parents first
        SceneObject o = *scene.find(id);
        const bool root = std::find(roots.begin(), roots.end(), id) != roots.end();
        o.id = ObjectId(c.objects.size() + 1);
        local[id] = o.id;
        if (root) {
            o.parent = kNoObject;
            auto w = Transform::fromMatrix(scene.worldMatrix(id));
            if (!w)
                throw std::invalid_argument("'" + o.name + "' cannot be copied on its own: its world transform is sheared by a non-uniformly scaled parent (copy that parent with it)");
            o.transform = *w;
        } else {
            o.parent = local.at(o.parent);
        }
        o.generator.reset();
        c.objects.push_back(std::move(o));
    }
    if (c.objects.size() > kMaxObjects) throw std::invalid_argument("too many objects to copy");
    return c;
}

Json Clipboard::toJson() const {
    Json j = Json::object();
    j["format"] = kFormat;
    j["version"] = kVersion;
    j["sourceProject"] = sourceProject;
    Json arr = Json::array();
    for (const auto& o : objects) arr.push_back(o.toJson());
    j["objects"] = arr;
    return j;
}

Clipboard Clipboard::fromJson(const Json& j) {
    if (!j.isObject() || j.getString("format") != kFormat) throw std::invalid_argument("not a TITAN object clipboard");
    const int version = int(j.getNumber("version", 0));
    if (version < 1 || version > kVersion)
        throw std::invalid_argument("clipboard version " + std::to_string(version) + " is not supported (this engine reads 1-" + std::to_string(kVersion) + ")");
    const Json& arr = j["objects"];
    if (!arr.isArray() || arr.size() == 0) throw std::invalid_argument("clipboard holds no objects");
    if (arr.size() > kMaxObjects) throw std::invalid_argument("clipboard holds too many objects");
    Clipboard c;
    c.sourceProject = j.getString("sourceProject");
    for (const auto& e : arr.asArray()) {
        SceneObject o = SceneObject::fromJson(e);
        if (o.id != ObjectId(c.objects.size() + 1)) throw std::invalid_argument("clipboard ids must be 1..n in order");
        if (o.parent != kNoObject && o.parent >= o.id) throw std::invalid_argument("clipboard parent links must point to earlier objects");
        if (!std::isfinite(o.transform.position.x) || !std::isfinite(o.transform.position.y) || !std::isfinite(o.transform.position.z) ||
            o.transform.scale.x == 0 || o.transform.scale.y == 0 || o.transform.scale.z == 0)
            throw std::invalid_argument("clipboard object '" + o.name + "' has an invalid transform");
        o.generator.reset();
        c.objects.push_back(std::move(o));
    }
    return c;
}

std::vector<SceneObject> Clipboard::paste(Scene& scene, ObjectId parent, bool sameProject) const {
    Mat4 toParent;
    if (parent != kNoObject) {
        if (!scene.contains(parent)) throw std::invalid_argument("parent not found");
        auto inv = scene.worldMatrix(parent).inverse();
        if (!inv) throw std::invalid_argument("parent has a degenerate transform");
        toParent = *inv;
    }
    // Every root placement is resolved before any id is allocated: a refused paste changes nothing.
    std::map<ObjectId, Transform> rootTransform;
    for (const SceneObject& src : objects) {
        if (src.parent != kNoObject) continue;
        if (parent == kNoObject) {
            rootTransform[src.id] = src.transform;
            continue;
        }
        auto t = Transform::fromMatrix(toParent * src.transform.matrix());
        if (!t)
            throw std::invalid_argument("'" + src.name + "' cannot keep its placement under that parent: it would be sheared by the parent's non-uniform scale; nothing was pasted");
        rootTransform[src.id] = *t;
    }
    std::map<ObjectId, ObjectId> sceneId;
    std::vector<SceneObject> out;
    for (const SceneObject& src : objects) {
        SceneObject o = src;
        o.id = scene.allocateId();
        sceneId[src.id] = o.id;
        if (src.parent == kNoObject) {
            o.parent = parent;
            o.transform = rootTransform.at(src.id);
        } else {
            o.parent = sceneId.at(src.parent);
        }
        if (!sameProject) o.prefab.reset();
        out.push_back(std::move(o));
    }
    return out;
}

}  // namespace titan
