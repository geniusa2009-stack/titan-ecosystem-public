// TITAN Engine — prefabs. Original TITAN code.
#include "titan/prefab.h"

#include "titan/project.h"

#include <cctype>
#include <chrono>
#include <map>
#include <random>
#include <set>
#include <stdexcept>

namespace titan {
namespace {
bool isHexId(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s)
        if (!std::isxdigit(uint8_t(c)) || std::isupper(uint8_t(c))) return false;
    return true;
}
}  // namespace

std::string newPrefabId() {
    static std::mt19937_64 rng{std::random_device{}() ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())};
    static const char* d = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 2; ++i) {
        const uint64_t v = rng();
        for (int k = 0; k < 16; ++k) s += d[(v >> (k * 4)) & 15];
    }
    return s;
}

Json Prefab::toJson() const {
    Json j = Json::object();
    j["format"] = kFormatVersion;
    j["id"] = id;
    j["name"] = name;
    Json arr = Json::array();
    for (const auto& o : objects) {
        Json oj = o.toJson();
        oj.erase("prefab");  // links belong to instances, not to the prefab itself
        arr.push_back(oj);
    }
    j["objects"] = arr;
    return j;
}

Prefab Prefab::fromJson(const Json& j) {
    if (!j.isObject()) throw std::invalid_argument("prefab must be a JSON object");
    if (j.getNumber("format", 0) != kFormatVersion)
        throw std::invalid_argument("unsupported prefab format " + std::to_string(int(j.getNumber("format", 0))));
    Prefab p;
    p.id = j.getString("id");
    p.name = j.getString("name");
    if (!isHexId(p.id)) throw std::invalid_argument("prefab id must be 32 lowercase hex characters");
    if (p.name.empty()) throw std::invalid_argument("prefab name is empty");
    const Json& arr = j["objects"];
    if (!arr.isArray() || arr.size() == 0) throw std::invalid_argument("prefab has no objects");
    if (arr.size() > kMaxObjects) throw std::invalid_argument("prefab has too many objects");
    std::set<ObjectId> ids;
    for (const auto& oj : arr.asArray()) {
        SceneObject o = SceneObject::fromJson(oj);
        o.prefab.reset();
        o.generator.reset();
        if (o.id == kNoObject || !ids.insert(o.id).second) throw std::invalid_argument("prefab object ids must be unique and non-zero");
        p.objects.push_back(std::move(o));
    }
    if (p.objects[0].parent != kNoObject) throw std::invalid_argument("the first prefab object must be the root (parent 0)");
    // Every other object's parent must appear earlier: one root, no cycles, no dangling parents.
    std::set<ObjectId> before{p.objects[0].id};
    for (size_t i = 1; i < p.objects.size(); ++i) {
        if (!before.count(p.objects[i].parent))
            throw std::invalid_argument("prefab object " + std::to_string(p.objects[i].id) + " has a parent outside the prefab or after it");
        before.insert(p.objects[i].id);
    }
    return p;
}

Prefab Prefab::fromSubtree(const Scene& scene, ObjectId root, const std::string& name, const std::string& id) {
    if (!scene.contains(root)) throw std::invalid_argument("object " + std::to_string(root) + " not found");
    const std::vector<ObjectId> tree = scene.withDescendants({root});  // parents first
    if (tree.size() > kMaxObjects) throw std::invalid_argument("too many objects for one prefab");
    std::map<ObjectId, ObjectId> local;
    for (ObjectId sid : tree) local[sid] = ObjectId(local.size() + 1);
    Prefab p;
    p.id = id;
    p.name = name;
    for (ObjectId sid : tree) {
        SceneObject o = *scene.find(sid);
        o.id = local[sid];
        o.parent = sid == root ? kNoObject : local.at(o.parent);
        o.generator.reset();
        o.prefab.reset();
        p.objects.push_back(std::move(o));
    }
    return p;
}

std::vector<SceneObject> Prefab::instantiate(Scene& scene, ObjectId parent, const std::optional<Vec3>& position) const {
    if (parent != kNoObject && !scene.contains(parent)) throw std::invalid_argument("parent not found");
    std::map<ObjectId, ObjectId> sceneId;
    std::vector<SceneObject> out;
    for (const SceneObject& src : objects) {
        SceneObject o = src;
        o.id = scene.allocateId();
        sceneId[src.id] = o.id;
        o.parent = src.parent == kNoObject ? parent : sceneId.at(src.parent);
        o.prefab = PrefabLink{id, src.id};
        if (src.parent == kNoObject && position) o.transform.position = *position;
        out.push_back(std::move(o));
    }
    return out;
}

Prefab readPrefabFile(const std::filesystem::path& file) { return Prefab::fromJson(readJsonFile(file)); }

}  // namespace titan
