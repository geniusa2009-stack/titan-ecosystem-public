// TITAN blackboard and behavior tree tests. Original TITAN code.
#include "titan/behavior_tree.h"
#include "titan_test.h"

#include <limits>

using namespace titan;
using namespace titan::ai;

namespace {
struct Trace {
    std::vector<std::string> events;
};

class Scripted : public Action {
public:
    Scripted(std::shared_ptr<Trace> t, std::string name, std::vector<std::string> script)
        : trace_(std::move(t)), name_(std::move(name)), script_(std::move(script)) {}
    Status tick(ActionContext&) override {
        trace_->events.push_back(name_ + ":tick");
        const std::string& s = script_[std::min(step_++, script_.size() - 1)];
        if (s == "throw") throw std::runtime_error(name_ + " exploded");
        return s == "success" ? Status::Success : (s == "failure" ? Status::Failure : Status::Running);
    }
    void abort(ActionContext&) override {
        trace_->events.push_back(name_ + ":abort");
        step_ = 0;
    }

private:
    std::shared_ptr<Trace> trace_;
    std::string name_;
    std::vector<std::string> script_;
    size_t step_ = 0;
};

std::shared_ptr<ActionRegistry> registry(std::shared_ptr<Trace> trace) {
    auto r = std::make_shared<ActionRegistry>();
    ActionType t;
    t.name = "scripted";
    t.create = [trace](const Json& p) {
        std::vector<std::string> script;
        for (const Json& s : p["script"].asArray()) script.push_back(s.asString());
        return std::make_unique<Scripted>(trace, p["name"].asString(), script);
    };
    t.validate = [](const Json& p, const BlackboardSchema&) {
        if (!p["name"].isString() || !p["script"].isArray() || p["script"].size() == 0)
            throw std::invalid_argument("needs 'name' and a non-empty 'script'");
        for (const Json& s : p["script"].asArray()) {
            const std::string v = s.asString();
            if (v != "success" && v != "failure" && v != "running" && v != "throw") throw std::invalid_argument("bad step");
        }
    };
    r->add(t);
    return r;
}

std::shared_ptr<BlackboardSchema> schema() {
    auto s = std::make_shared<BlackboardSchema>();
    s->add({"hp", BbType::Float, BbValue(100.0)});
    s->add({"alerted", BbType::Bool, BbValue(false)});
    s->add({"target", BbType::Entity, std::nullopt});
    s->add({"count", BbType::Int, BbValue(int64_t(0))});
    s->add({"label", BbType::String, std::nullopt});
    return s;
}

Json obj(std::initializer_list<std::pair<const char*, Json>> kv) {
    Json o = Json::object();
    for (const auto& [k, v] : kv) o[k] = v;
    return o;
}
Json script(const char* name, std::initializer_list<const char*> steps) {
    Json s = Json::array();
    for (const char* st : steps) s.push_back(st);
    return obj({{"type", "scripted"}, {"params", obj({{"name", name}, {"script", s}})}});
}
BtNode node(int64_t id, const char* kind, std::vector<int64_t> children = {}, Json params = Json::object()) {
    BtNode n;
    n.id = id;
    n.kind = kind;
    n.children = std::move(children);
    n.params = std::move(params);
    return n;
}

struct Rig {
    std::shared_ptr<Trace> trace = std::make_shared<Trace>();
    std::shared_ptr<ActionRegistry> reg = registry(trace);
    std::shared_ptr<BlackboardSchema> sch = schema();
    std::shared_ptr<const BehaviorTree> tree;
    std::unique_ptr<BehaviorTreeInstance> inst;
    Blackboard bb{sch};
    Rig(std::vector<BtNode> nodes, int64_t root) {
        tree = std::make_shared<BehaviorTree>(sch, std::move(nodes), root, reg.get());
        inst = std::make_unique<BehaviorTreeInstance>(tree, reg, 42);
    }
    Status tick(uint32_t budget = 1024) {
        TickBudget b;
        b.maxNodeVisits = budget;
        return inst->tick(bb, b).status;
    }
};
}  // namespace

TEST("blackboard: typed keys, missing values, validation and serialization") {
    BlackboardSchema s;
    s.add({"speed", BbType::Float, BbValue(1.5)});
    CHECK_THROWS(s.add({"speed", BbType::Float, std::nullopt}));
    CHECK_THROWS(s.add({"bad name", BbType::Bool, std::nullopt}));
    CHECK_THROWS(s.add({"n", BbType::Int, BbValue(2.0)}));  // initial of the wrong type
    CHECK_THROWS(s.add({"f", BbType::Float, BbValue(std::numeric_limits<double>::quiet_NaN())}));
    s.add({"home", BbType::Vec3, BbValue(DVec3{1, 2, 3})});
    s.add({"who", BbType::Entity, std::nullopt});
    s.add({"n", BbType::Int, std::nullopt});
    auto shared = std::make_shared<BlackboardSchema>(s);
    Blackboard bb(shared);
    CHECK(bb.number("speed") == 1.5 && !bb.has("who") && !bb.number("n"));
    const uint64_t rev = bb.revision();
    CHECK_THROWS(bb.set("speed", BbValue(int64_t(2))));
    CHECK_THROWS(bb.set("missing", BbValue(true)));
    CHECK_THROWS(bb.set("n", BbValue((int64_t(1) << 53) + 1)));
    CHECK_THROWS(bb.set("who", BbValue(EntityRef{0})));
    CHECK_THROWS(bb.set("home", BbValue(DVec3{0, std::numeric_limits<double>::infinity(), 0})));
    CHECK(bb.revision() == rev && *bb.number("speed") == 1.5);
    bb.set("who", BbValue(EntityRef{77}));
    bb.set("n", BbValue(int64_t(-4)));
    CHECK(bb.number("n") == -4.0 && std::get<EntityRef>(*bb.get("who")).id == 77);
    CHECK_THROWS(bb.number("who"));
    bb.unset("speed");
    CHECK(!bb.has("speed"));
    const Blackboard back = Blackboard::fromJson(Json::parse(bb.toJson().dump()), shared);
    CHECK(back.toJson() == bb.toJson() && !back.has("speed"));
    Json unknown = bb.toJson();
    unknown["values"]["ghost"] = 1;
    CHECK_THROWS(Blackboard::fromJson(unknown, shared));
    Json mistyped = bb.toJson();
    mistyped["values"]["n"] = 1.5;
    CHECK_THROWS(Blackboard::fromJson(mistyped, shared));
    CHECK(BlackboardSchema::fromJson(Json::parse(s.toJson().dump())).toJson() == s.toJson());
    bb.resetToInitial();
    CHECK(*bb.number("speed") == 1.5 && !bb.has("who"));
}

TEST("behavior tree: validation rejects malformed graphs with clear errors") {
    auto trace = std::make_shared<Trace>();
    auto reg = registry(trace);
    auto sch = schema();
    auto build = [&](std::vector<BtNode> nodes, int64_t root, const ActionRegistry* r) {
        return BehaviorTree(sch, std::move(nodes), root, r);
    };
    const Json ok = script("a", {"success"});
    auto bad = [&](std::vector<BtNode> nodes, int64_t root = 1, bool withRegistry = true) {
        bool threw = false;
        try {
            build(std::move(nodes), root, withRegistry ? reg.get() : nullptr);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        return threw;
    };
    CHECK(bad({node(1, "sequence", {2}), node(2, "action", {}, ok), node(2, "action", {}, ok)}));  // duplicate id
    CHECK(bad({node(1, "parallel", {})}));
    CHECK(bad({node(1, "sequence", {9})}));
    CHECK(bad({node(1, "sequence", {2, 3}), node(2, "action", {}, ok), node(3, "sequence", {2})}));  // shared child
    CHECK(bad({node(1, "sequence", {2}), node(2, "action", {}, ok), node(3, "action", {}, ok)}));     // unreachable
    CHECK(bad({node(1, "sequence", {})}));
    CHECK(bad({node(1, "inverter", {2, 3}), node(2, "action", {}, ok), node(3, "action", {}, ok)}));
    CHECK(bad({node(1, "action", {2}, ok), node(2, "action", {}, ok)}));
    CHECK(bad({node(1, "repeat", {2}, obj({{"count", 0}})), node(2, "action", {}, ok)}));
    CHECK(bad({node(1, "repeat", {2}, obj({{"count", 1.5}})), node(2, "action", {}, ok)}));
    CHECK(bad({node(1, "condition", {}, obj({{"key", "mana"}, {"op", "isSet"}}))}));
    CHECK(bad({node(1, "condition", {}, obj({{"key", "alerted"}, {"op", "lt"}, {"value", 1}}))}));
    CHECK(bad({node(1, "condition", {}, obj({{"key", "hp"}, {"op", "eq"}, {"value", "lots"}}))}));
    CHECK(bad({node(1, "condition", {}, obj({{"key", "hp"}, {"op", "approx"}, {"value", 1}}))}));
    CHECK(bad({node(1, "set", {}, obj({{"key", "count"}, {"value", 2.5}}))}));
    CHECK(bad({node(1, "action", {}, obj({{"type", "teleport"}}))}));
    CHECK(bad({node(1, "action", {}, ok)}, 1, false));  // actions need a registry
    CHECK(bad({node(1, "action", {}, script("a", {"sometimes"}))}));
    CHECK(bad({node(1, "action", {}, ok)}, 5));          // missing root
    std::vector<BtNode> deep;
    for (int64_t i = 1; i <= 65; ++i) deep.push_back(node(i, "inverter", {i + 1}));
    deep.push_back(node(66, "action", {}, ok));
    CHECK(bad(deep));
    std::string message;
    try {
        build({node(1, "sequence", {2}), node(2, "inverter", {1})}, 1, reg.get());
    } catch (const std::invalid_argument& e) {
        message = e.what();
    }
    CHECK(message.find("cycle") != std::string::npos);
    std::vector<BtNode> fine;
    for (int64_t i = 1; i <= 63; ++i) fine.push_back(node(i, "inverter", {i + 1}));
    fine.push_back(node(64, "action", {}, ok));
    build(fine, 1, reg.get());  // exactly the depth limit is accepted
}

TEST("behavior tree: composites with memory, conditions on missing data, interruption and restart") {
    Rig rig({node(1, "selector", {2, 5}),
             node(2, "sequence", {3, 4}),
             node(3, "condition", {}, obj({{"key", "hp"}, {"op", "lt"}, {"value", 50}})),
             node(4, "action", {}, script("flee", {"running", "success"})),
             node(5, "action", {}, script("patrol", {"running"}))},
            1);
    CHECK(rig.tick() == Status::Running);
    rig.bb.set("hp", BbValue(10.0));
    CHECK(rig.tick() == Status::Running);  // memory: the running patrol is resumed, not re-decided
    const std::vector<std::string> first{"patrol:tick", "patrol:tick"};
    CHECK(rig.trace->events == first);
    CHECK(rig.inst->running());
    rig.inst->abort(rig.bb);
    CHECK(!rig.inst->running());
    CHECK(rig.tick() == Status::Running);
    CHECK(rig.tick() == Status::Success);
    const std::vector<std::string> all{"patrol:tick", "patrol:tick", "patrol:abort", "flee:tick", "flee:tick"};
    CHECK(rig.trace->events == all);
    rig.inst->abort(rig.bb);  // nothing running: no further abort calls
    CHECK(rig.trace->events.size() == all.size());
    rig.bb.unset("hp");  // missing information never satisfies "hp < 50"
    rig.trace->events.clear();
    rig.tick();
    CHECK(rig.trace->events.size() == 1 && rig.trace->events[0] == "patrol:tick");
}

TEST("behavior tree: decorators, waits and blackboard writes") {
    {
        Rig r({node(1, "inverter", {2}), node(2, "condition", {}, obj({{"key", "alerted"}, {"op", "isTrue"}}))}, 1);
        CHECK(r.tick() == Status::Success);
    }
    {
        Rig r({node(1, "forceFailure", {2}), node(2, "action", {}, script("a", {"success"}))}, 1);
        CHECK(r.tick() == Status::Failure);
    }
    {
        Rig r({node(1, "forceSuccess", {2}), node(2, "action", {}, script("a", {"failure"}))}, 1);
        CHECK(r.tick() == Status::Success);
    }
    {
        Rig r({node(1, "repeat", {2}, obj({{"count", 3}})), node(2, "action", {}, script("a", {"success"}))}, 1);
        CHECK(r.tick() == Status::Running && r.tick() == Status::Running && r.tick() == Status::Success);
        CHECK(r.trace->events.size() == 3);
    }
    {
        Rig r({node(1, "retry", {2}, obj({{"count", 2}})), node(2, "action", {}, script("a", {"failure"}))}, 1);
        CHECK(r.tick() == Status::Running && r.tick() == Status::Failure);
    }
    {
        Rig r({node(1, "timeout", {2}, obj({{"ticks", 3}})), node(2, "action", {}, script("slow", {"running"}))}, 1);
        CHECK(r.tick() == Status::Running && r.tick() == Status::Running && r.tick() == Status::Failure);
        const std::vector<std::string> want{"slow:tick", "slow:tick", "slow:tick", "slow:abort"};
        CHECK(r.trace->events == want && !r.inst->running());
    }
    {
        Rig r({node(1, "guard", {2}, obj({{"condition", obj({{"key", "alerted"}, {"op", "isTrue"}})}})),
               node(2, "action", {}, script("hunt", {"running"}))},
              1);
        CHECK(r.tick() == Status::Failure && r.trace->events.empty());
        r.bb.set("alerted", BbValue(true));
        CHECK(r.tick() == Status::Running);
        r.bb.set("alerted", BbValue(false));
        CHECK(r.tick() == Status::Failure);
        const std::vector<std::string> want{"hunt:tick", "hunt:abort"};
        CHECK(r.trace->events == want);
    }
    {
        Rig r({node(1, "sequence", {2, 3, 4}), node(2, "wait", {}, obj({{"ticks", 2}})),
               node(3, "set", {}, obj({{"key", "count"}, {"value", 5}})), node(4, "clear", {}, obj({{"key", "hp"}}))},
              1);
        CHECK(r.tick() == Status::Running && r.bb.number("count") == 0.0);
        CHECK(r.tick() == Status::Success);
        CHECK(r.bb.number("count") == 5.0 && !r.bb.has("hp"));
    }
}

TEST("behavior tree: node-visit budget spreads work over ticks without repeating side effects") {
    std::vector<BtNode> nodes{node(1, "sequence", {2, 3, 4, 5, 6, 7, 8, 9, 10, 11})};
    for (int64_t i = 2; i <= 11; ++i) {
        const std::string name = "a" + std::to_string(i - 2);
        nodes.push_back(node(i, "action", {}, script(name.c_str(), {"success"})));
    }
    Rig r(nodes, 1);
    TickBudget b;
    b.maxNodeVisits = 4;
    TickResult t1 = r.inst->tick(r.bb, b);
    CHECK(t1.status == Status::Running && t1.budgetExhausted && t1.visits == 4);
    int ticks = 1;
    TickResult t;
    do {
        t = r.inst->tick(r.bb, b);
        ++ticks;
    } while (t.status == Status::Running && ticks < 10);
    CHECK(t.status == Status::Success && ticks == 4 && !t.budgetExhausted);
    CHECK(r.trace->events.size() == 10);
    for (size_t i = 0; i < 10; ++i) CHECK(r.trace->events[i] == "a" + std::to_string(i) + ":tick");
    TickBudget zero;
    zero.maxNodeVisits = 0;
    CHECK_THROWS(r.inst->tick(r.bb, zero));
}

TEST("behavior tree: action exceptions fail safely; instances are deterministic and schema-bound") {
    Rig r({node(1, "selector", {2, 3}), node(2, "action", {}, script("bad", {"throw"})),
           node(3, "action", {}, script("good", {"success"}))},
          1);
    CHECK(r.tick() == Status::Success);
    CHECK(r.inst->lastError().find("bad exploded") != std::string::npos);
    auto other = std::make_shared<BlackboardSchema>(*r.sch);  // equal content, different schema object
    Blackboard foreign(other);
    CHECK_THROWS(r.inst->tick(foreign));

    auto run = [] {
        Rig x({node(1, "sequence", {2, 3}), node(2, "repeat", {4}, obj({{"count", 2}})),
               node(4, "action", {}, script("step", {"running", "success"})),
               node(3, "set", {}, obj({{"key", "label"}, {"value", "done"}}))},
              1);
        std::vector<std::string> statuses;
        for (int i = 0; i < 6; ++i) statuses.push_back(toString(x.tick()));
        statuses.insert(statuses.end(), x.trace->events.begin(), x.trace->events.end());
        statuses.push_back(x.bb.toJson().dump());
        return statuses;
    };
    CHECK(run() == run());
}

TEST("behavior tree: asset serialization round trip and refusals") {
    auto trace = std::make_shared<Trace>();
    auto reg = registry(trace);
    BehaviorTree tree(schema(),
                      {node(10, "selector", {20, 30}),
                       node(20, "guard", {21}, obj({{"condition", obj({{"key", "target"}, {"op", "isSet"}})}})),
                       node(21, "action", {}, script("chase", {"running"})),
                       node(30, "wait", {}, obj({{"ticks", 5}}))},
                      10, reg.get());
    const Json j = tree.toJson();
    const BehaviorTree back = BehaviorTree::fromJson(Json::parse(j.dump()), reg.get());
    CHECK(back.toJson() == j && back.root() == 10);
    Json v2 = j;
    v2["version"] = 2;
    CHECK_THROWS(BehaviorTree::fromJson(v2, reg.get()));
    Json cyc = j;
    cyc["nodes"].asArray()[2]["kind"] = "inverter";
    cyc["nodes"].asArray()[2]["children"] = Json(Json::Array{Json(20)});
    CHECK_THROWS(BehaviorTree::fromJson(cyc, reg.get()));
    CHECK_THROWS(BehaviorTree::fromJson(j, nullptr));  // has actions, no registry
}

TITAN_TEST_MAIN()
