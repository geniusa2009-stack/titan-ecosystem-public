// TITAN visual scripting core tests. Original TITAN code.
#include "titan/script_graph.h"
#include "titan_test.h"

#include <stdexcept>

using namespace titan;

namespace {
ScriptNode node(int64_t id, const char* kind, Json params) {
    ScriptNode n;
    n.id = id;
    n.kind = kind;
    n.params = std::move(params);
    return n;
}
Json obj(std::initializer_list<std::pair<const char*, Json>> kv) {
    Json o = Json::object();
    for (const auto& [k, v] : kv) o[k] = v;
    return o;
}
// on "tick": score += 10; if score >= 30 emit "win" else emit "continue"
ScriptGraph scoreGraph() {
    ScriptGraph g;
    g.nodes.push_back(node(1, "event", obj({{"name", "tick"}})));
    g.nodes.push_back(node(2, "add", obj({{"var", "score"}, {"value", 10}})));
    g.nodes.push_back(node(3, "branch", obj({{"var", "score"}, {"op", "ge"}, {"value", 30}})));
    g.nodes.push_back(node(4, "emit", obj({{"event", "win"}})));
    g.nodes.push_back(node(5, "emit", obj({{"event", "continue"}})));
    g.links = {{1, "out", 2}, {2, "out", 3}, {3, "true", 4}, {3, "false", 5}};
    return g;
}
}  // namespace

TEST("script: events, conditions and variables execute deterministically") {
    const ScriptGraph g = scoreGraph();
    std::map<std::string, double> vars;
    ScriptResult r = runScript(g, "tick", vars);
    CHECK(vars.empty());  // the caller's variables are never changed
    CHECK(r.vars.at("score") == 10);
    CHECK(r.emitted.size() == 1 && r.emitted[0] == "continue");
    CHECK(r.steps == 4);
    r = runScript(g, "tick", r.vars);
    r = runScript(g, "tick", r.vars);
    CHECK(r.vars.at("score") == 30);
    CHECK(r.emitted.size() == 1 && r.emitted[0] == "win");
    const ScriptResult other = runScript(g, "jump", r.vars);
    CHECK(other.steps == 0 && other.emitted.empty() && other.vars == r.vars);
}

TEST("script: matching event handlers run in ascending node id order") {
    ScriptGraph g;
    g.nodes.push_back(node(7, "event", obj({{"name", "go"}})));
    g.nodes.push_back(node(8, "emit", obj({{"event", "second"}})));
    g.nodes.push_back(node(2, "event", obj({{"name", "go"}})));
    g.nodes.push_back(node(3, "emit", obj({{"event", "first"}})));
    g.links = {{7, "out", 8}, {2, "out", 3}};
    const ScriptResult r = runScript(g, "go");
    CHECK(r.emitted.size() == 2 && r.emitted[0] == "first" && r.emitted[1] == "second");
}

TEST("script: validation refuses unsafe or malformed graphs") {
    ScriptGraph cyc = scoreGraph();
    cyc.links.push_back({4, "out", 2});
    CHECK_THROWS(cyc.validate());
    ScriptGraph fanOut = scoreGraph();
    fanOut.links.push_back({1, "out", 5});
    CHECK_THROWS(fanOut.validate());
    ScriptGraph badPin = scoreGraph();
    badPin.links.push_back({2, "true", 4});
    CHECK_THROWS(badPin.validate());
    ScriptGraph intoEvent = scoreGraph();
    intoEvent.links.push_back({4, "out", 1});
    CHECK_THROWS(intoEvent.validate());
    ScriptGraph missing = scoreGraph();
    missing.links.push_back({5, "out", 99});
    CHECK_THROWS(missing.validate());
    ScriptGraph dup = scoreGraph();
    dup.nodes.push_back(node(2, "emit", obj({{"event", "x"}})));
    CHECK_THROWS(dup.validate());
    ScriptGraph badOp = scoreGraph();
    badOp.nodes[2].params["op"] = "approx";
    CHECK_THROWS(badOp.validate());
    ScriptGraph badKind = scoreGraph();
    badKind.nodes[1].kind = "shell";
    CHECK_THROWS(badKind.validate());
    ScriptGraph badValue = scoreGraph();
    badValue.nodes[1].params["value"] = "ten";
    CHECK_THROWS(badValue.validate());
    CHECK_THROWS(runScript(cyc, "tick"));
}

TEST("script: step limit stops long chains without partial results") {
    const ScriptGraph g = scoreGraph();
    bool threw = false;
    try {
        (void)runScript(g, "tick", {}, 3);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(runScript(g, "tick", {}, 4).steps == 4);
}

TEST("script: json round trip preserves the graph and validates input") {
    const ScriptGraph g = scoreGraph();
    const Json j = g.toJson();
    const ScriptGraph back = ScriptGraph::fromJson(Json::parse(j.dump()));
    CHECK(back.toJson() == j);
    CHECK(runScript(back, "tick").emitted == runScript(g, "tick").emitted);
    Json wrongVersion = j;
    wrongVersion["version"] = 2;
    CHECK_THROWS(ScriptGraph::fromJson(wrongVersion));
    Json cyclic = j;
    Json link = Json::object();
    link["from"] = 4;
    link["to"] = 2;
    cyclic["links"].push_back(link);
    CHECK_THROWS(ScriptGraph::fromJson(cyclic));
}

TITAN_TEST_MAIN()
