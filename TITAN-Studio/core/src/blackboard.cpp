// TITAN Engine — typed AI blackboard. Original TITAN code.
#include "titan/blackboard.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace titan::ai {
namespace {

constexpr int64_t kMaxInt = int64_t(1) << 53;
void fail(const std::string& msg) { throw std::invalid_argument(msg); }
void checkVersion(const Json& j, const char* format) {
    if (!j.isObject() || j.getString("format") != format) throw JsonError(std::string("not a ") + format + " document");
    const Json* v = j.find("version");
    if (!v || !v->isNumber() || v->asNumber() != 1) throw JsonError(std::string("unsupported ") + format + " version");
}

}  // namespace

const char* toString(BbType t) {
    switch (t) {
        case BbType::Bool: return "bool";
        case BbType::Int: return "int";
        case BbType::Float: return "float";
        case BbType::String: return "string";
        case BbType::Vec3: return "vec3";
        case BbType::Entity: return "entity";
    }
    return "float";
}

BbType bbTypeFrom(const std::string& s) {
    for (BbType t : {BbType::Bool, BbType::Int, BbType::Float, BbType::String, BbType::Vec3, BbType::Entity})
        if (s == toString(t)) return t;
    throw JsonError("unknown blackboard type '" + s + "'");
}

BbType typeOf(const BbValue& v) { return BbType(v.index()); }

void validateValue(const BbValue& v) {
    switch (typeOf(v)) {
        case BbType::Bool: return;
        case BbType::Int: {
            const int64_t i = std::get<int64_t>(v);
            if (i > kMaxInt || i < -kMaxInt) fail("int blackboard values must be within +/- 2^53");
            return;
        }
        case BbType::Float:
            if (!std::isfinite(std::get<double>(v))) fail("float blackboard values must be finite");
            return;
        case BbType::String:
            if (std::get<std::string>(v).size() > 4096) fail("string blackboard values are limited to 4096 bytes");
            return;
        case BbType::Vec3: {
            const DVec3& p = std::get<DVec3>(v);
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) fail("vec3 blackboard values must be finite");
            return;
        }
        case BbType::Entity: {
            const uint64_t id = std::get<EntityRef>(v).id;
            if (id == 0 || id >= uint64_t(kMaxInt)) fail("entity ids must be in [1, 2^53)");
            return;
        }
    }
}

Json valueToJson(const BbValue& v) {
    switch (typeOf(v)) {
        case BbType::Bool: return Json(std::get<bool>(v));
        case BbType::Int: return Json(std::get<int64_t>(v));
        case BbType::Float: return Json(std::get<double>(v));
        case BbType::String: return Json(std::get<std::string>(v));
        case BbType::Vec3: {
            const DVec3& p = std::get<DVec3>(v);
            return Json(Json::Array{Json(p.x), Json(p.y), Json(p.z)});
        }
        case BbType::Entity: return Json(std::get<EntityRef>(v).id);
    }
    return Json();
}

BbValue valueFromJson(const Json& j, BbType type) {
    BbValue v;
    switch (type) {
        case BbType::Bool: v = j.asBool(); break;
        case BbType::Int: v = j.asInt(); break;
        case BbType::Float: v = j.asNumber(); break;
        case BbType::String: v = j.asString(); break;
        case BbType::Vec3:
            if (!j.isArray() || j.size() != 3) throw JsonError("vec3 values must be [x, y, z]");
            v = DVec3{j.at(0).asNumber(), j.at(1).asNumber(), j.at(2).asNumber()};
            break;
        case BbType::Entity: v = EntityRef{j.asUInt()}; break;
    }
    validateValue(v);
    return v;
}

bool isValidKeyName(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    });
}

// ---- schema --------------------------------------------------------------------------------------------------------

void BlackboardSchema::add(const BbKey& key) {
    if (!isValidKeyName(key.name)) fail("invalid blackboard key name '" + key.name + "'");
    if (index_.count(key.name)) fail("duplicate blackboard key '" + key.name + "'");
    if (key.initial) {
        if (typeOf(*key.initial) != key.type)
            fail("initial value of '" + key.name + "' is not of type " + toString(key.type));
        validateValue(*key.initial);
    }
    if (keys_.size() >= 4096) throw std::length_error("blackboard schemas are limited to 4096 keys");
    index_[key.name] = keys_.size();
    keys_.push_back(key);
}

const BbKey* BlackboardSchema::find(const std::string& name) const {
    auto it = index_.find(name);
    return it == index_.end() ? nullptr : &keys_[it->second];
}

Json BlackboardSchema::toJson() const {
    Json j = Json::object();
    j["format"] = "titan.blackboard-schema";
    j["version"] = 1;
    Json ks = Json::array();
    for (const BbKey& k : keys_) {
        Json o = Json::object();
        o["name"] = k.name;
        o["type"] = toString(k.type);
        if (k.initial) o["initial"] = valueToJson(*k.initial);
        ks.push_back(std::move(o));
    }
    j["keys"] = std::move(ks);
    return j;
}

BlackboardSchema BlackboardSchema::fromJson(const Json& j) {
    checkVersion(j, "titan.blackboard-schema");
    if (!j["keys"].isArray()) throw JsonError("blackboard schema needs a 'keys' array");
    BlackboardSchema s;
    for (const Json& o : j["keys"].asArray()) {
        if (!o.isObject()) throw JsonError("blackboard key must be an object");
        BbKey k;
        k.name = o["name"].asString();
        k.type = bbTypeFrom(o["type"].asString());
        if (const Json* init = o.find("initial")) k.initial = valueFromJson(*init, k.type);
        s.add(k);
    }
    return s;
}

// ---- blackboard ----------------------------------------------------------------------------------------------------

Blackboard::Blackboard(std::shared_ptr<const BlackboardSchema> schema) : schema_(std::move(schema)) {
    if (!schema_) fail("blackboard needs a schema");
    resetToInitial();
}

size_t Blackboard::slot(const std::string& key) const {
    const BbKey* k = schema_->find(key);
    if (!k) fail("unknown blackboard key '" + key + "'");
    return size_t(k - schema_->keys().data());
}

void Blackboard::set(const std::string& key, const BbValue& value) {
    const size_t s = slot(key);
    const BbType want = schema_->keys()[s].type;
    if (typeOf(value) != want)
        fail("blackboard key '" + key + "' is " + toString(want) + ", not " + toString(typeOf(value)));
    validateValue(value);
    values_[s] = value;
    ++revision_;
}

void Blackboard::unset(const std::string& key) {
    values_[slot(key)].reset();
    ++revision_;
}

const BbValue* Blackboard::get(const std::string& key) const {
    const auto& v = values_[slot(key)];
    return v ? &*v : nullptr;
}

std::optional<double> Blackboard::number(const std::string& key) const {
    const BbValue* v = get(key);
    if (!v) return std::nullopt;
    if (const auto* i = std::get_if<int64_t>(v)) return double(*i);
    if (const auto* d = std::get_if<double>(v)) return *d;
    fail("blackboard key '" + key + "' is not numeric");
    return std::nullopt;
}

void Blackboard::resetToInitial() {
    values_.assign(schema_->size(), std::nullopt);
    for (size_t i = 0; i < schema_->size(); ++i) values_[i] = schema_->keys()[i].initial;
    ++revision_;
}

Json Blackboard::toJson() const {
    Json j = Json::object();
    j["format"] = "titan.blackboard";
    j["version"] = 1;
    Json vs = Json::object();
    for (size_t i = 0; i < values_.size(); ++i)
        if (values_[i]) vs[schema_->keys()[i].name] = valueToJson(*values_[i]);
    j["values"] = std::move(vs);
    return j;
}

Blackboard Blackboard::fromJson(const Json& j, std::shared_ptr<const BlackboardSchema> schema) {
    checkVersion(j, "titan.blackboard");
    Blackboard b(std::move(schema));
    const Json& vs = j["values"];
    if (!vs.isObject()) throw JsonError("blackboard needs a 'values' object");
    b.values_.assign(b.schema_->size(), std::nullopt);  // only what the document holds is set
    for (const auto& [name, value] : vs.asObject()) {
        const size_t s = b.slot(name);
        if (b.values_[s]) fail("duplicate blackboard value '" + name + "'");
        b.values_[s] = valueFromJson(value, b.schema_->keys()[s].type);
    }
    b.revision_ = 0;
    return b;
}

}  // namespace titan::ai
