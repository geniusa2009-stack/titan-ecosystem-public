// TITAN Engine — visual scripting core. Original TITAN code.
#include "titan/script_graph.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace titan {
namespace {

constexpr int64_t kMaxNodeId = (int64_t(1) << 53) - 1;

void fail(const std::string& msg) { throw std::invalid_argument(msg); }

std::string nodeName(const ScriptNode& n) { return "node " + std::to_string(n.id) + " (" + n.kind + ")"; }

const std::string& stringParam(const ScriptNode& n, const char* key) {
    const Json* v = n.params.find(key);
    if (!v || !v->isString() || v->asString().empty() || v->asString().size() > 128)
        fail(nodeName(n) + ": '" + key + "' must be a string of 1-128 characters");
    return v->asString();
}

double numberParam(const ScriptNode& n, const char* key) {
    const Json* v = n.params.find(key);
    if (!v || !v->isNumber() || !std::isfinite(v->asNumber())) fail(nodeName(n) + ": '" + key + "' must be a finite number");
    return v->asNumber();
}

bool knownOp(const std::string& op) {
    return op == "lt" || op == "le" || op == "gt" || op == "ge" || op == "eq" || op == "ne";
}

std::vector<std::string> pinsOf(const std::string& kind) {
    if (kind == "branch") return {"true", "false"};
    return {"out"};
}

void validateNode(const ScriptNode& n) {
    if (!n.params.isObject()) fail(nodeName(n) + ": params must be an object");
    if (n.kind == "event") {
        stringParam(n, "name");
    } else if (n.kind == "set" || n.kind == "add") {
        stringParam(n, "var");
        numberParam(n, "value");
    } else if (n.kind == "branch") {
        stringParam(n, "var");
        numberParam(n, "value");
        if (!knownOp(stringParam(n, "op"))) fail(nodeName(n) + ": unknown op '" + n.params["op"].asString() + "'");
    } else if (n.kind == "emit") {
        stringParam(n, "event");
    } else {
        fail("node " + std::to_string(n.id) + ": unknown kind '" + n.kind + "'");
    }
}

bool compare(double a, const std::string& op, double b) {
    if (op == "lt") return a < b;
    if (op == "le") return a <= b;
    if (op == "gt") return a > b;
    if (op == "ge") return a >= b;
    if (op == "eq") return a == b;
    return a != b;
}

}  // namespace

void ScriptGraph::validate() const {
    std::map<int64_t, const ScriptNode*> byId;
    for (const auto& n : nodes) {
        if (n.id <= 0 || n.id > kMaxNodeId) fail("node ids must be positive integers below 2^53");
        if (!byId.emplace(n.id, &n).second) fail("duplicate node id " + std::to_string(n.id));
        validateNode(n);
    }
    std::set<std::pair<int64_t, std::string>> used;
    std::map<int64_t, std::vector<int64_t>> next;
    for (const auto& l : links) {
        auto from = byId.find(l.from);
        auto to = byId.find(l.to);
        if (from == byId.end() || to == byId.end())
            fail("link " + std::to_string(l.from) + " -> " + std::to_string(l.to) + " references a missing node");
        const auto pins = pinsOf(from->second->kind);
        if (std::find(pins.begin(), pins.end(), l.pin) == pins.end())
            fail(nodeName(*from->second) + " has no output pin '" + l.pin + "'");
        if (!used.emplace(l.from, l.pin).second)
            fail(nodeName(*from->second) + ": pin '" + l.pin + "' has more than one link");
        if (to->second->kind == "event") fail(nodeName(*to->second) + " cannot be the target of a link");
        next[l.from].push_back(l.to);
    }
    // Cycle check (iterative DFS, colours: 0 new, 1 on stack, 2 done).
    std::map<int64_t, int> colour;
    for (const auto& [start, node] : byId) {
        (void)node;
        if (colour[start] != 0) continue;
        std::vector<std::pair<int64_t, size_t>> stack{{start, 0}};
        colour[start] = 1;
        while (!stack.empty()) {
            auto& [id, i] = stack.back();
            const auto& out = next[id];
            if (i < out.size()) {
                const int64_t to = out[i++];
                if (colour[to] == 1) fail("execution links form a cycle through node " + std::to_string(to));
                if (colour[to] == 0) {
                    colour[to] = 1;
                    stack.emplace_back(to, 0);
                }
            } else {
                colour[id] = 2;
                stack.pop_back();
            }
        }
    }
}

Json ScriptGraph::toJson() const {
    Json j = Json::object();
    j["format"] = "titan.script";
    j["version"] = 1;
    Json ns = Json::array();
    for (const auto& n : nodes) {
        Json o = Json::object();
        o["id"] = n.id;
        o["kind"] = n.kind;
        o["params"] = n.params;
        ns.push_back(std::move(o));
    }
    j["nodes"] = std::move(ns);
    Json ls = Json::array();
    for (const auto& l : links) {
        Json o = Json::object();
        o["from"] = l.from;
        o["pin"] = l.pin;
        o["to"] = l.to;
        ls.push_back(std::move(o));
    }
    j["links"] = std::move(ls);
    return j;
}

ScriptGraph ScriptGraph::fromJson(const Json& j) {
    if (!j.isObject() || j.getString("format") != "titan.script") throw JsonError("not a titan.script document");
    const Json* version = j.find("version");
    if (!version || !version->isNumber() || version->asNumber() != 1) throw JsonError("unsupported titan.script version");
    ScriptGraph g;
    const Json& ns = j["nodes"];
    const Json& ls = j["links"];
    if (!ns.isArray() || !ls.isArray()) throw JsonError("titan.script needs 'nodes' and 'links' arrays");
    for (const Json& n : ns.asArray()) {
        if (!n.isObject()) throw JsonError("script node must be an object");
        ScriptNode node;
        node.id = n["id"].asInt();
        node.kind = n["kind"].asString();
        node.params = n.contains("params") ? n["params"] : Json::object();
        g.nodes.push_back(std::move(node));
    }
    for (const Json& l : ls.asArray()) {
        if (!l.isObject()) throw JsonError("script link must be an object");
        ScriptLink link;
        link.from = l["from"].asInt();
        link.pin = l.getString("pin", "out");
        link.to = l["to"].asInt();
        g.links.push_back(std::move(link));
    }
    g.validate();
    return g;
}

ScriptResult runScript(const ScriptGraph& graph, const std::string& event, const std::map<std::string, double>& vars,
                       int maxSteps) {
    graph.validate();
    std::map<int64_t, const ScriptNode*> byId;
    for (const auto& n : graph.nodes) byId[n.id] = &n;
    std::map<std::pair<int64_t, std::string>, int64_t> next;
    for (const auto& l : graph.links) next[{l.from, l.pin}] = l.to;

    ScriptResult r;
    r.vars = vars;
    for (const auto& [startId, start] : byId) {  // ascending id: deterministic order of handlers
        if (start->kind != "event" || start->params["name"].asString() != event) continue;
        int64_t cur = startId;
        while (cur != 0) {
            if (++r.steps > maxSteps) throw std::runtime_error("script exceeded " + std::to_string(maxSteps) + " steps");
            const ScriptNode& n = *byId.at(cur);
            std::string pin = "out";
            if (n.kind == "set") {
                r.vars[n.params["var"].asString()] = n.params["value"].asNumber();
            } else if (n.kind == "add") {
                r.vars[n.params["var"].asString()] += n.params["value"].asNumber();
            } else if (n.kind == "branch") {
                auto it = r.vars.find(n.params["var"].asString());
                const double v = it == r.vars.end() ? 0.0 : it->second;
                pin = compare(v, n.params["op"].asString(), n.params["value"].asNumber()) ? "true" : "false";
            } else if (n.kind == "emit") {
                r.emitted.push_back(n.params["event"].asString());
            }
            auto it = next.find({cur, pin});
            cur = it == next.end() ? 0 : it->second;
        }
    }
    return r;
}

}  // namespace titan
