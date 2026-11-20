// TITAN Engine — material system: typed base materials, parameters and instances with overrides.
// Original TITAN code.
//
// A base material declares typed parameters with defaults. An instance names a parent (base or another instance)
// and overrides a subset of the inherited parameters; it cannot add parameters or change their types. Parents
// must be added before their children, so inheritance chains are always acyclic (depth is limited to 16).
// Value encoding (JSON):
//   float   - finite number, optionally within the parameter's [min, max]
//   color   - [r, g, b], finite and >= 0 (HDR allowed)
//   bool    - true / false
//   texture - asset reference string (0-256 characters, "" means none)
// Not exposed over the protocol yet: no capability is advertised until it is wired and verified.
#pragma once

#include "titan/json.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace titan {

enum class ParamType { Float, Color, Bool, Texture };

struct MaterialParam {
    std::string name;
    ParamType type = ParamType::Float;
    Json value;  // default
    std::optional<double> min, max;  // floats only
};

struct Material {
    std::string name;
    std::string parent;                    // empty for a base material
    std::vector<MaterialParam> params;     // base materials only
    std::map<std::string, Json> overrides;  // instances only
    bool isInstance() const { return !parent.empty(); }
};

class MaterialLibrary {
public:
    // std::invalid_argument (library unchanged): bad or duplicate name, unknown parent, chain deeper than 16,
    // params on an instance or overrides on a base, unknown/duplicate parameters, or values of the wrong type.
    void add(Material m);
    const Material* find(const std::string& name) const;
    size_t size() const { return order_.size(); }

    // Declaration of `param` as seen by `material` (inherited from its base), or nullptr.
    const MaterialParam* paramOf(const std::string& material, const std::string& param) const;
    // Every parameter's effective value: base defaults, then overrides from the root instance down to `name`.
    std::map<std::string, Json> resolve(const std::string& name) const;
    // Sets one override on an instance after validating it; throws std::invalid_argument and changes nothing
    // otherwise.
    void setOverride(const std::string& instance, const std::string& param, const Json& value);

    // {"format":"titan.materials","version":1,"materials":[...]} in insertion order (parents first).
    Json toJson() const;
    static MaterialLibrary fromJson(const Json& j);

private:
    const Material& base(const Material& m) const;
    std::map<std::string, Material> byName_;
    std::vector<std::string> order_;
};

}  // namespace titan
