// TITAN Engine — material system. Original TITAN code.
#include "titan/material.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace titan {
namespace {

constexpr int kMaxDepth = 16;

void fail(const std::string& msg) { throw std::invalid_argument(msg); }

bool validName(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
               c == '-' || c == '/';
    });
}

const char* typeName(ParamType t) {
    switch (t) {
        case ParamType::Float: return "float";
        case ParamType::Color: return "color";
        case ParamType::Bool: return "bool";
        case ParamType::Texture: return "texture";
    }
    return "float";
}
ParamType typeFrom(const std::string& s) {
    if (s == "float") return ParamType::Float;
    if (s == "color") return ParamType::Color;
    if (s == "bool") return ParamType::Bool;
    if (s == "texture") return ParamType::Texture;
    throw JsonError("unknown material parameter type '" + s + "'");
}

void checkValue(const std::string& where, const MaterialParam& p, const Json& v) {
    switch (p.type) {
        case ParamType::Float: {
            if (!v.isNumber() || !std::isfinite(v.asNumber())) fail(where + ": expected a finite number");
            if ((p.min && v.asNumber() < *p.min) || (p.max && v.asNumber() > *p.max)) fail(where + ": value out of range");
            return;
        }
        case ParamType::Color: {
            bool ok = v.isArray() && v.size() == 3;
            for (size_t i = 0; ok && i < 3; ++i)
                ok = v.at(i).isNumber() && std::isfinite(v.at(i).asNumber()) && v.at(i).asNumber() >= 0;
            if (!ok) fail(where + ": expected [r, g, b] with finite, non-negative components");
            return;
        }
        case ParamType::Bool:
            if (!v.isBool()) fail(where + ": expected true or false");
            return;
        case ParamType::Texture:
            if (!v.isString() || v.asString().size() > 256) fail(where + ": expected an asset reference string");
            return;
    }
}

}  // namespace

const Material* MaterialLibrary::find(const std::string& name) const {
    auto it = byName_.find(name);
    return it == byName_.end() ? nullptr : &it->second;
}

const Material& MaterialLibrary::base(const Material& m) const {
    const Material* cur = &m;
    while (cur->isInstance()) cur = &byName_.at(cur->parent);
    return *cur;
}

const MaterialParam* MaterialLibrary::paramOf(const std::string& material, const std::string& param) const {
    const Material* m = find(material);
    if (!m) return nullptr;
    for (const auto& p : base(*m).params)
        if (p.name == param) return &p;
    return nullptr;
}

void MaterialLibrary::add(Material m) {
    if (!validName(m.name)) fail("invalid material name '" + m.name + "'");
    if (byName_.count(m.name)) fail("duplicate material '" + m.name + "'");
    const std::string where = "material '" + m.name + "'";
    if (!m.isInstance()) {
        if (!m.overrides.empty()) fail(where + ": a base material cannot have overrides");
        std::set<std::string> names;
        for (const auto& p : m.params) {
            if (!validName(p.name)) fail(where + ": invalid parameter name '" + p.name + "'");
            if (!names.insert(p.name).second) fail(where + ": duplicate parameter '" + p.name + "'");
            if (p.type != ParamType::Float && (p.min || p.max)) fail(where + ": only float parameters have a range");
            if ((p.min && !std::isfinite(*p.min)) || (p.max && !std::isfinite(*p.max)) || (p.min && p.max && *p.min > *p.max))
                fail(where + ": invalid range for '" + p.name + "'");
            checkValue(where + " parameter '" + p.name + "'", p, p.value);
        }
    } else {
        const Material* parent = find(m.parent);
        if (!parent) fail(where + ": unknown parent '" + m.parent + "'");
        if (!m.params.empty()) fail(where + ": an instance cannot declare parameters");
        int depth = 1;
        for (const Material* p = parent; p->isInstance(); p = find(p->parent))
            if (++depth > kMaxDepth) fail(where + ": instance chain is deeper than 16");
        const Material& root = base(*parent);
        for (const auto& [name, value] : m.overrides) {
            auto it = std::find_if(root.params.begin(), root.params.end(),
                                   [&](const MaterialParam& p) { return p.name == name; });
            if (it == root.params.end()) fail(where + ": '" + root.name + "' has no parameter '" + name + "'");
            checkValue(where + " override '" + name + "'", *it, value);
        }
    }
    order_.push_back(m.name);
    byName_.emplace(m.name, std::move(m));
}

std::map<std::string, Json> MaterialLibrary::resolve(const std::string& name) const {
    const Material* m = find(name);
    if (!m) fail("unknown material '" + name + "'");
    std::vector<const Material*> chain;
    for (const Material* cur = m; cur; cur = cur->isInstance() ? find(cur->parent) : nullptr) chain.push_back(cur);
    std::map<std::string, Json> out;
    for (const auto& p : chain.back()->params) out[p.name] = p.value;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        for (const auto& [k, v] : (*it)->overrides) out[k] = v;
    return out;
}

void MaterialLibrary::setOverride(const std::string& instance, const std::string& param, const Json& value) {
    auto it = byName_.find(instance);
    if (it == byName_.end()) fail("unknown material '" + instance + "'");
    if (!it->second.isInstance()) fail("material '" + instance + "' is not an instance");
    const MaterialParam* p = paramOf(instance, param);
    if (!p) fail("material '" + instance + "' has no parameter '" + param + "'");
    checkValue("material '" + instance + "' override '" + param + "'", *p, value);
    it->second.overrides[param] = value;
}

Json MaterialLibrary::toJson() const {
    Json j = Json::object();
    j["format"] = "titan.materials";
    j["version"] = 1;
    Json ms = Json::array();
    for (const auto& name : order_) {
        const Material& m = byName_.at(name);
        Json o = Json::object();
        o["name"] = m.name;
        if (m.isInstance()) {
            o["parent"] = m.parent;
            Json ov = Json::object();
            for (const auto& [k, v] : m.overrides) ov[k] = v;
            o["overrides"] = std::move(ov);
        } else {
            Json ps = Json::array();
            for (const auto& p : m.params) {
                Json pj = Json::object();
                pj["name"] = p.name;
                pj["type"] = typeName(p.type);
                pj["value"] = p.value;
                if (p.min) pj["min"] = *p.min;
                if (p.max) pj["max"] = *p.max;
                ps.push_back(std::move(pj));
            }
            o["params"] = std::move(ps);
        }
        ms.push_back(std::move(o));
    }
    j["materials"] = std::move(ms);
    return j;
}

MaterialLibrary MaterialLibrary::fromJson(const Json& j) {
    if (!j.isObject() || j.getString("format") != "titan.materials") throw JsonError("not a titan.materials document");
    const Json* version = j.find("version");
    if (!version || !version->isNumber() || version->asNumber() != 1) throw JsonError("unsupported titan.materials version");
    if (!j["materials"].isArray()) throw JsonError("titan.materials needs a 'materials' array");
    MaterialLibrary lib;
    for (const Json& o : j["materials"].asArray()) {
        if (!o.isObject()) throw JsonError("material must be an object");
        Material m;
        m.name = o["name"].asString();
        m.parent = o.getString("parent");
        if (const Json* ps = o.find("params")) {
            if (!ps->isArray()) throw JsonError("material 'params' must be an array");
            for (const Json& pj : ps->asArray()) {
                if (!pj.isObject()) throw JsonError("material parameter must be an object");
                MaterialParam p;
                p.name = pj["name"].asString();
                p.type = typeFrom(pj["type"].asString());
                p.value = pj["value"];
                if (pj.contains("min")) p.min = pj["min"].asNumber();
                if (pj.contains("max")) p.max = pj["max"].asNumber();
                m.params.push_back(std::move(p));
            }
        }
        if (const Json* ov = o.find("overrides")) {
            if (!ov->isObject()) throw JsonError("material 'overrides' must be an object");
            for (const auto& [k, v] : ov->asObject()) {
                if (m.overrides.count(k)) throw JsonError("duplicate override '" + k + "'");
                m.overrides[k] = v;
            }
        }
        lib.add(std::move(m));
    }
    return lib;
}

}  // namespace titan
