// TITAN Engine — input mapping. Original TITAN code.
#include "titan/input_map.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace titan {
namespace {

void fail(const std::string& msg) { throw std::invalid_argument(msg); }

bool validName(const std::string& s, size_t max) {
    if (s.empty() || s.size() > max) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    });
}

const char* kindName(ActionKind k) { return k == ActionKind::Axis ? "axis" : "button"; }
ActionKind kindFrom(const std::string& s) {
    if (s == "button") return ActionKind::Button;
    if (s == "axis") return ActionKind::Axis;
    throw JsonError("unknown input action kind '" + s + "'");
}

}  // namespace

bool isValidControlName(const std::string& control) {
    const auto dot = control.find('.');
    if (dot == std::string::npos) return false;
    const std::string device = control.substr(0, dot);
    const std::string name = control.substr(dot + 1);
    if (device != "Keyboard" && device != "Mouse" && device != "Gamepad") return false;
    return validName(name, 32) && name.find('.') == std::string::npos;
}

void InputMap::validate() const {
    std::set<std::string> actionNames;
    for (const auto& a : actions) {
        if (!validName(a.name, 64)) fail("invalid input action name '" + a.name + "'");
        if (!actionNames.insert(a.name).second) fail("duplicate input action '" + a.name + "'");
    }
    std::set<std::string> contextNames;
    for (const auto& c : contexts) {
        if (!validName(c.name, 64)) fail("invalid input context name '" + c.name + "'");
        if (!contextNames.insert(c.name).second) fail("duplicate input context '" + c.name + "'");
        std::set<std::pair<std::string, std::string>> bound;
        for (const auto& b : c.bindings) {
            const std::string where = "context '" + c.name + "': ";
            if (!isValidControlName(b.control)) fail(where + "invalid control '" + b.control + "'");
            if (!actionNames.count(b.action)) fail(where + "unknown action '" + b.action + "'");
            if (!std::isfinite(b.scale) || b.scale == 0) fail(where + "binding scale must be finite and non-zero");
            if (!bound.emplace(b.control, b.action).second)
                fail(where + "'" + b.control + "' is bound to '" + b.action + "' twice");
        }
    }
}

const InputContext* InputMap::findContext(const std::string& name) const {
    for (const auto& c : contexts)
        if (c.name == name) return &c;
    return nullptr;
}

Json InputMap::toJson() const {
    Json j = Json::object();
    j["format"] = "titan.input";
    j["version"] = 1;
    Json as = Json::array();
    for (const auto& a : actions) {
        Json o = Json::object();
        o["name"] = a.name;
        o["kind"] = kindName(a.kind);
        as.push_back(std::move(o));
    }
    j["actions"] = std::move(as);
    Json cs = Json::array();
    for (const auto& c : contexts) {
        Json o = Json::object();
        o["name"] = c.name;
        o["priority"] = c.priority;
        Json bs = Json::array();
        for (const auto& b : c.bindings) {
            Json bj = Json::object();
            bj["control"] = b.control;
            bj["action"] = b.action;
            bj["scale"] = b.scale;
            bs.push_back(std::move(bj));
        }
        o["bindings"] = std::move(bs);
        cs.push_back(std::move(o));
    }
    j["contexts"] = std::move(cs);
    return j;
}

InputMap InputMap::fromJson(const Json& j) {
    if (!j.isObject() || j.getString("format") != "titan.input") throw JsonError("not a titan.input document");
    const Json* version = j.find("version");
    if (!version || !version->isNumber() || version->asNumber() != 1) throw JsonError("unsupported titan.input version");
    if (!j["actions"].isArray() || !j["contexts"].isArray()) throw JsonError("titan.input needs 'actions' and 'contexts'");
    InputMap m;
    for (const Json& a : j["actions"].asArray()) {
        if (!a.isObject()) throw JsonError("input action must be an object");
        m.actions.push_back({a["name"].asString(), kindFrom(a.getString("kind", "button"))});
    }
    for (const Json& c : j["contexts"].asArray()) {
        if (!c.isObject() || !c["bindings"].isArray()) throw JsonError("input context must be an object with 'bindings'");
        InputContext ctx;
        ctx.name = c["name"].asString();
        const int64_t priority = c.contains("priority") ? c["priority"].asInt() : 0;
        if (priority < -1000000 || priority > 1000000) throw JsonError("input context priority out of range");
        ctx.priority = int(priority);
        for (const Json& b : c["bindings"].asArray()) {
            if (!b.isObject()) throw JsonError("input binding must be an object");
            ctx.bindings.push_back({b["control"].asString(), b["action"].asString(), float(b.getNumber("scale", 1.0))});
        }
        m.contexts.push_back(std::move(ctx));
    }
    m.validate();
    return m;
}

InputRouter::InputRouter(InputMap map) : map_(std::move(map)) { map_.validate(); }

void InputRouter::push(const std::string& context) {
    if (!map_.findContext(context)) fail("unknown input context '" + context + "'");
    if (std::find(active_.begin(), active_.end(), context) != active_.end())
        fail("input context '" + context + "' is already active");
    active_.push_back(context);
}

void InputRouter::pop(const std::string& context) {
    auto it = std::find(active_.begin(), active_.end(), context);
    if (it == active_.end()) fail("input context '" + context + "' is not active");
    active_.erase(it);
}

std::map<std::string, float> InputRouter::evaluate(const std::map<std::string, float>& controls) const {
    for (const auto& [control, value] : controls)
        if (!std::isfinite(value)) fail("control '" + control + "' has a non-finite value");

    // Highest priority first; among equal priorities the most recently pushed wins (stable sort of reversed order).
    std::vector<const InputContext*> order;
    for (auto it = active_.rbegin(); it != active_.rend(); ++it) order.push_back(map_.findContext(*it));
    std::stable_sort(order.begin(), order.end(),
                     [](const InputContext* a, const InputContext* b) { return a->priority > b->priority; });

    std::map<std::string, float> axis;
    std::map<std::string, bool> pressed;
    std::set<std::string> claimed;
    for (const InputContext* ctx : order) {
        std::set<std::string> claimedHere;
        for (const auto& b : ctx->bindings) {
            if (claimed.count(b.control)) continue;  // a higher-priority context owns this control
            claimedHere.insert(b.control);
            auto v = controls.find(b.control);
            if (v == controls.end()) continue;
            const float x = v->second * b.scale;
            axis[b.action] += x;
            if (std::fabs(x) >= 0.5f) pressed[b.action] = true;
        }
        claimed.insert(claimedHere.begin(), claimedHere.end());
    }

    std::map<std::string, float> out;
    for (const auto& a : map_.actions) {
        if (a.kind == ActionKind::Button) {
            out[a.name] = pressed.count(a.name) ? 1.0f : 0.0f;
        } else {
            auto it = axis.find(a.name);
            out[a.name] = it == axis.end() ? 0.0f : std::clamp(it->second, -1.0f, 1.0f);
        }
    }
    return out;
}

}  // namespace titan
