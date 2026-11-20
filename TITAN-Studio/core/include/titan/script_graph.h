// TITAN Engine — visual scripting core: a validated node graph with events, conditions and deterministic
// execution. Original TITAN code.
//
// A graph is a set of nodes joined by execution links (from node + output pin -> to node). Execution starts at
// every "event" node whose name matches the fired event (ascending node id) and follows one link per step.
// Validation guarantees termination and determinism: every (node, pin) has at most one outgoing link, event nodes
// have no incoming links and the execution graph is acyclic. Variables are numbers.
//
// Node kinds (params in braces):
//   event  {name}                    pins: out
//   set    {var, value}              pins: out          var = value
//   add    {var, value}              pins: out          var += value (missing var counts as 0)
//   branch {var, op, value}          pins: true, false  op is one of lt, le, gt, ge, eq, ne
//   emit   {event}                   pins: out          appends `event` to the result's emitted list
// Not exposed over the protocol yet: no capability is advertised until it is wired and verified.
#pragma once

#include "titan/json.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace titan {

struct ScriptNode {
    int64_t id = 0;  // > 0, unique within the graph
    std::string kind;
    Json params = Json::object();
};

struct ScriptLink {
    int64_t from = 0;
    std::string pin = "out";
    int64_t to = 0;
};

struct ScriptGraph {
    std::vector<ScriptNode> nodes;
    std::vector<ScriptLink> links;

    // Throws std::invalid_argument describing the first problem found.
    void validate() const;

    // {"format":"titan.script","version":1,"nodes":[...],"links":[...]}; fromJson validates.
    Json toJson() const;
    static ScriptGraph fromJson(const Json& j);
};

struct ScriptResult {
    std::map<std::string, double> vars;
    std::vector<std::string> emitted;
    int steps = 0;  // nodes executed
};

// Fires `event` on a validated copy of `graph`. Never changes `vars`; the result carries the new values.
// Throws std::invalid_argument for an invalid graph, std::runtime_error when more than `maxSteps` nodes run.
ScriptResult runScript(const ScriptGraph& graph, const std::string& event,
                       const std::map<std::string, double>& vars = {}, int maxSteps = 10000);

}  // namespace titan
