// TITAN Engine — typed AI blackboard: a schema of named, typed keys and per-agent values. Original TITAN code.
//
// Shared by behavior trees (titan/behavior_tree.h), utility decisions and perception. A key is declared once in a
// BlackboardSchema with a type and an optional initial value; every Blackboard bound to the schema accepts only
// values of the declared type. A key may be unset, which is how "unknown / missing information" is represented.
//
// Types: Bool, Int (|v| <= 2^53 so it round-trips through JSON), Float (finite), String (<= 4096 bytes),
// Vec3 (finite components), Entity (an id in [1, 2^53), stable across saves).
// Thread safety: none; a blackboard belongs to one agent and is used by one thread.
#pragma once

#include "titan/json.h"
#include "titan/spatial.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace titan::ai {

using spatial::DVec3;

struct EntityRef {
    uint64_t id = 0;
    bool operator==(const EntityRef& o) const { return id == o.id; }
};

enum class BbType { Bool, Int, Float, String, Vec3, Entity };
using BbValue = std::variant<bool, int64_t, double, std::string, DVec3, EntityRef>;

const char* toString(BbType t);
BbType bbTypeFrom(const std::string& s);  // JsonError for unknown names
BbType typeOf(const BbValue& v);
void validateValue(const BbValue& v);  // std::invalid_argument for out-of-range content
Json valueToJson(const BbValue& v);
BbValue valueFromJson(const Json& j, BbType type);  // validated
bool isValidKeyName(const std::string& name);  // 1-64 characters of [A-Za-z0-9_.]

struct BbKey {
    std::string name;
    BbType type = BbType::Float;
    std::optional<BbValue> initial;  // unset when absent
};

class BlackboardSchema {
public:
    void add(const BbKey& key);  // std::invalid_argument: bad name, duplicate, mistyped or invalid initial value
    const BbKey* find(const std::string& name) const;
    const std::vector<BbKey>& keys() const { return keys_; }
    size_t size() const { return keys_.size(); }

    // {"format":"titan.blackboard-schema","version":1,"keys":[{"name","type","initial"?}]}
    Json toJson() const;
    static BlackboardSchema fromJson(const Json& j);

private:
    std::vector<BbKey> keys_;
    std::map<std::string, size_t> index_;
};

class Blackboard {
public:
    explicit Blackboard(std::shared_ptr<const BlackboardSchema> schema);  // std::invalid_argument for null

    // std::invalid_argument for unknown keys, a type mismatch or an invalid value (nothing changes).
    void set(const std::string& key, const BbValue& value);
    void unset(const std::string& key);
    const BbValue* get(const std::string& key) const;  // nullptr when unset; throws for unknown keys
    bool has(const std::string& key) const { return get(key) != nullptr; }
    std::optional<double> number(const std::string& key) const;  // Int or Float as double; nullopt when unset
    void resetToInitial();
    uint64_t revision() const { return revision_; }  // bumps on every change
    const BlackboardSchema& schema() const { return *schema_; }

    // {"format":"titan.blackboard","version":1,"values":{name: value, ...}} (unset keys omitted); fromJson validates
    // every value against the schema and refuses unknown keys.
    Json toJson() const;
    static Blackboard fromJson(const Json& j, std::shared_ptr<const BlackboardSchema> schema);

private:
    size_t slot(const std::string& key) const;
    std::shared_ptr<const BlackboardSchema> schema_;
    std::vector<std::optional<BbValue>> values_;
    uint64_t revision_ = 0;
};

}  // namespace titan::ai
