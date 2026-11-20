// TITAN Engine — component registry (see titan/components.h). Original TITAN code.
#include "titan/components.h"

#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace titan {

namespace {
void validateTags(const Json& data) {
    const Json& tags = data["tags"];
    if (!tags.isArray()) throw std::invalid_argument("titan.tags: \"tags\" must be an array of strings");
    if (tags.size() > 256) throw std::invalid_argument("titan.tags: at most 256 tags");
    std::set<std::string> seen;
    for (const auto& t : tags.asArray()) {
        if (!t.isString() || t.asString().empty() || t.asString().size() > 64)
            throw std::invalid_argument("titan.tags: each tag must be a string of 1-64 characters");
        if (!seen.insert(t.asString()).second) throw std::invalid_argument("titan.tags: duplicate tag '" + t.asString() + "'");
    }
}
}  // namespace

ComponentRegistry ComponentRegistry::withBuiltins() {
    ComponentRegistry r;
    ComponentType tags;
    tags.type = "titan.tags";
    tags.version = 1;
    tags.validate = validateTags;
    r.add(std::move(tags));
    return r;
}

void ComponentRegistry::add(ComponentType t) {
    if (t.type.empty()) throw std::invalid_argument("component type name is empty");
    if (t.version < 1) throw std::invalid_argument("component '" + t.type + "' version must be at least 1");
    if (t.version > 1 && !t.migrate)
        throw std::invalid_argument("component '" + t.type + "' needs a migration from older versions");
    if (types_.count(t.type)) throw std::invalid_argument("component '" + t.type + "' is already registered");
    const std::string name = t.type;
    types_.emplace(name, std::move(t));
}

const ComponentType* ComponentRegistry::find(const std::string& type) const {
    auto it = types_.find(type);
    return it == types_.end() ? nullptr : &it->second;
}

Component ComponentRegistry::normalize(const Component& c) const {
    const ComponentType* t = find(c.type);
    if (!t) return c;
    if (c.version > t->version)
        throw std::invalid_argument("component '" + c.type + "' version " + std::to_string(c.version) +
                                    " is newer than this engine supports (" + std::to_string(t->version) + ")");
    Component out = c;
    if (c.version < t->version) {
        out.data = t->migrate(c.data, c.version);
        out.version = t->version;
    }
    if (!out.data.isObject()) throw std::invalid_argument("component '" + c.type + "' data must be an object");
    if (t->validate) t->validate(out.data);
    return out;
}

void ComponentRegistry::normalizeScene(Scene& scene) const {
    std::vector<SceneObject> changed;
    for (const auto& entry : scene.objects()) {
        SceneObject n = entry.second;
        for (auto& c : n.components) c = normalize(c);
        if (n != entry.second) changed.push_back(std::move(n));
    }
    for (const auto& o : changed) scene.put(o);
}

}  // namespace titan
