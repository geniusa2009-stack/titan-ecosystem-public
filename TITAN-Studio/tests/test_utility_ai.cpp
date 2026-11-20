// TITAN utility decision tests: curves, validation, scoring, selection rules over time, explanations and state.
// Original TITAN code.
#include "titan/utility_ai.h"
#include "titan_test.h"

#include <cmath>
#include <limits>

using namespace titan;
using namespace titan::ai;

namespace {
const double kNan = std::numeric_limits<double>::quiet_NaN();

Json obj(std::initializer_list<std::pair<const char*, Json>> kv) {
    Json o = Json::object();
    for (const auto& [k, v] : kv) o[k] = v;
    return o;
}

ResponseCurve linear(double slope = 1, double intercept = 0, bool invert = false) {
    ResponseCurve c;
    c.slope = slope;
    c.intercept = intercept;
    c.invert = invert;
    return c;
}

Consideration consider(const char* name, const char* key, double min, double max, double weight = 1,
                       ResponseCurve curve = linear()) {
    Consideration c;
    c.name = name;
    c.key = key;
    c.min = min;
    c.max = max;
    c.weight = weight;
    c.curve = curve;
    return c;
}

std::shared_ptr<BlackboardSchema> combatSchema() {
    auto s = std::make_shared<BlackboardSchema>();
    s->add({"hp", BbType::Float, BbValue(100.0)});
    s->add({"ammo", BbType::Int, std::nullopt});
    s->add({"enemyVisible", BbType::Bool, BbValue(false)});
    s->add({"distance", BbType::Float, std::nullopt});
    s->add({"a", BbType::Float, BbValue(0.0)});
    s->add({"b", BbType::Float, BbValue(0.0)});
    s->add({"enabled", BbType::Bool, BbValue(true)});
    s->add({"name", BbType::String, std::nullopt});
    return s;
}

UtilityAction attackAction() {
    UtilityAction a;
    a.name = "attack";
    a.considerations = {consider("sees", "enemyVisible", 0, 1), consider("ammo", "ammo", 0, 30),
                        consider("close", "distance", 0, 50, 2, linear(1, 0, true))};
    return a;
}
}  // namespace

TEST("utility: response curves evaluate, clamp and validate") {
    CHECK_NEAR(linear(2, -0.5).evaluate(0.5), 0.5, 1e-12);
    CHECK(linear(2, -0.5).evaluate(1) == 1 && linear(2, -0.5).evaluate(0) == 0);  // clamped output
    CHECK_NEAR(linear(1, 0, true).evaluate(0.2), 0.8, 1e-12);
    CHECK(linear().evaluate(-3) == 0 && linear().evaluate(7) == 1 && linear().evaluate(kNan) == 0);  // clamped input
    ResponseCurve pw;
    pw.kind = ResponseCurve::Kind::Power;
    pw.exponent = 2;
    CHECK_NEAR(pw.evaluate(0.5), 0.25, 1e-12);
    ResponseCurve lg;
    lg.kind = ResponseCurve::Kind::Logistic;
    CHECK_NEAR(lg.evaluate(0.5), 0.5, 1e-12);
    CHECK(lg.evaluate(1) > 0.99 && lg.evaluate(0) < 0.01);
    ResponseCurve st;
    st.kind = ResponseCurve::Kind::Step;
    st.threshold = 0.3;
    st.low = 0.1;
    st.high = 0.9;
    CHECK(st.evaluate(0.3) == 0.9 && st.evaluate(0.29) == 0.1);
    ResponseCurve pc;
    pc.kind = ResponseCurve::Kind::Piecewise;
    pc.points = {{0, 0}, {0.5, 1}, {1, 0.5}};
    CHECK_NEAR(pc.evaluate(0.25), 0.5, 1e-12);
    CHECK_NEAR(pc.evaluate(0.75), 0.75, 1e-12);
    for (const ResponseCurve& c : {linear(2, -0.5, true), pw, lg, st, pc})
        CHECK(ResponseCurve::fromJson(Json::parse(c.toJson().dump())).toJson() == c.toJson());
    ResponseCurve bad = pw;
    bad.exponent = 0;
    CHECK_THROWS(bad.validate());
    bad = linear(kNan);
    CHECK_THROWS(bad.validate());
    bad = lg;
    bad.steepness = 1000;
    CHECK_THROWS(bad.validate());
    bad = pc;
    bad.points = {{0.5, 1}};
    CHECK_THROWS(bad.validate());
    bad.points = {{0.5, 1}, {0.2, 0}};
    CHECK_THROWS(bad.validate());
    bad.points = {{0, 1.5}, {1, 0}};
    CHECK_THROWS(bad.validate());
}

TEST("utility: malformed models fail with the offending action named") {
    auto s = combatSchema();
    auto build = [&](UtilityAction a) { return UtilityModel(s, {std::move(a)}); };
    auto bad = [&](auto edit) {
        UtilityAction a = attackAction();
        edit(a);
        bool threw = false;
        try {
            build(a);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        return threw;
    };
    CHECK(bad([](UtilityAction& a) { a.considerations[0].key = "mana"; }));
    CHECK(bad([](UtilityAction& a) { a.considerations[0].key = "name"; }));  // strings are not scorable
    CHECK(bad([](UtilityAction& a) { a.considerations[1].min = 30; }));      // min == max
    CHECK(bad([](UtilityAction& a) { a.considerations[1].weight = -1; }));
    CHECK(bad([](UtilityAction& a) { a.considerations[1].weight = kNan; }));
    CHECK(bad([](UtilityAction& a) { a.considerations[1].missingValue = 2; }));
    CHECK(bad([](UtilityAction& a) { a.considerations[1].name = "sees"; }));  // duplicate consideration
    CHECK(bad([](UtilityAction& a) { a.considerations[1].curve.slope = kNan; }));
    CHECK(bad([](UtilityAction& a) { a.weight = 11; }));
    CHECK(bad([](UtilityAction& a) { a.cooldownSeconds = -1; }));
    CHECK(bad([](UtilityAction& a) { a.name = "has space"; }));
    CHECK(bad([](UtilityAction& a) { a.prerequisites.push_back(obj({{"key", "mana"}, {"op", "isSet"}})); }));
    CHECK(bad([](UtilityAction& a) {
        for (int i = 0; i < 33; ++i) a.considerations.push_back(consider(("c" + std::to_string(i)).c_str(), "hp", 0, 1));
    }));
    CHECK_THROWS(UtilityModel(s, {attackAction(), attackAction()}));  // duplicate action
    CHECK_THROWS(UtilityModel(s, {}));
    CHECK_THROWS(UtilityModel(s, {attackAction()}, kNan));
    std::string message;
    try {
        UtilityAction a = attackAction();
        a.considerations[2].max = -1;
        build(a);
    } catch (const std::invalid_argument& e) {
        message = e.what();
    }
    CHECK(message.find("attack") != std::string::npos && message.find("close") != std::string::npos);
}

TEST("utility: weighted geometric scoring, missing data policies and explanations") {
    auto s = combatSchema();
    Blackboard bb(s);
    bb.set("enemyVisible", BbValue(true));
    bb.set("ammo", BbValue(int64_t(15)));
    bb.set("distance", BbValue(10.0));
    const UtilityAction a = attackAction();
    const ActionEval ev = UtilityReasoner::evaluate(a, bb);
    CHECK_NEAR(ev.score, std::pow(1.0 * 0.5 * 0.8 * 0.8, 0.25), 1e-12);  // weights 1, 1, 2
    CHECK(ev.considerations.size() == 3);
    CHECK(ev.considerations[1].input == 15.0 && ev.considerations[1].normalized == 0.5);
    CHECK_NEAR(ev.considerations[2].score, 0.8, 1e-12);
    bb.unset("ammo");
    const ActionEval noAmmo = UtilityReasoner::evaluate(a, bb);
    CHECK(noAmmo.score == 0 && noAmmo.considerations[1].missing && !noAmmo.considerations[1].input);
    UtilityAction lenient = a;
    lenient.considerations[1].missing = Consideration::Missing::Value;
    lenient.considerations[1].missingValue = 0.5;
    CHECK_NEAR(UtilityReasoner::evaluate(lenient, bb).score, ev.score, 1e-12);
    lenient.considerations[1].missing = Consideration::Missing::One;
    CHECK_NEAR(UtilityReasoner::evaluate(lenient, bb).score, std::pow(0.64, 0.25), 1e-12);
    UtilityAction ignoreAmmo = a;
    ignoreAmmo.considerations[1].weight = 0;  // weight 0: neither vetoes nor counts
    CHECK_NEAR(UtilityReasoner::evaluate(ignoreAmmo, bb).score, std::pow(0.64, 1.0 / 3.0), 1e-12);
    UtilityAction plain;
    plain.name = "idle";
    plain.weight = 0.3;
    CHECK(UtilityReasoner::evaluate(plain, bb).score == 0.3);
}

TEST("utility: deterministic selection with prerequisites and stable tie-breaking") {
    auto s = combatSchema();
    UtilityAction flee;
    flee.name = "flee";
    flee.prerequisites = {obj({{"key", "hp"}, {"op", "lt"}, {"value", 30}})};
    flee.considerations = {consider("hurt", "hp", 0, 100, 1, linear(1, 0, true))};
    UtilityAction patrol;
    patrol.name = "patrol";
    patrol.weight = 0.3;
    UtilityAction guard = patrol;
    guard.name = "guard";  // same score as patrol, defined later
    UtilityAction watch = patrol;
    watch.name = "watch";
    watch.priority = 5;  // same score, higher priority
    {
        auto model = std::make_shared<UtilityModel>(s, std::vector<UtilityAction>{attackAction(), flee, patrol, guard});
        UtilityReasoner r(model);
        Blackboard bb(s);
        Decision d = r.decide(bb, 0);
        CHECK(d.action == "patrol" && d.reason == "started" && !d.switched);  // patrol beats guard by order
        CHECK(d.evaluations[0].blockedBy == "score" && d.evaluations[1].blockedBy == "prerequisite 0");
        bb.set("hp", BbValue(10.0));
        d = r.decide(bb, 1);
        CHECK(d.action == "flee" && d.switched && d.reason == "switched");
    }
    {
        auto model = std::make_shared<UtilityModel>(s, std::vector<UtilityAction>{patrol, guard, watch});
        UtilityReasoner r(model);
        Blackboard bb(s);
        CHECK(r.decide(bb, 0).action == "watch");  // priority breaks the score tie
    }
    {
        UtilityAction never = patrol;
        never.name = "never";
        never.weight = 0;
        auto model = std::make_shared<UtilityModel>(s, std::vector<UtilityAction>{never});
        UtilityReasoner r(model);
        Blackboard bb(s);
        const Decision d = r.decide(bb, 0);
        CHECK(d.action.empty() && d.reason == "none eligible" && r.current().empty());
    }
}

TEST("utility: commitment, hysteresis, cooldowns and ineligibility over time") {
    auto s = combatSchema();
    UtilityAction A;
    A.name = "A";
    A.minCommitSeconds = 2;
    A.cooldownSeconds = 5;
    A.considerations = {consider("a", "a", 0, 1)};
    UtilityAction B;
    B.name = "B";
    B.prerequisites = {obj({{"key", "enabled"}, {"op", "isTrue"}})};
    B.considerations = {consider("b", "b", 0, 1)};
    auto model = std::make_shared<UtilityModel>(s, std::vector<UtilityAction>{A, B}, 0.0, 0.1);
    UtilityReasoner r(model);
    Blackboard bb(s);
    auto step = [&](double t, double a, double b) {
        bb.set("a", BbValue(a));
        bb.set("b", BbValue(b));
        return r.decide(bb, t);
    };
    Decision d = step(0, 0.5, 0.3);
    CHECK(d.action == "A" && d.reason == "started");
    d = step(1, 0.5, 0.9);
    CHECK(d.action == "A" && d.reason == "commitment");
    d = step(3, 0.5, 0.55);
    CHECK(d.action == "A" && d.reason == "hysteresis");  // 0.55 does not beat 0.5 + 0.1
    d = step(4, 0.5, 0.7);
    CHECK(d.action == "B" && d.reason == "switched" && d.switched);
    d = step(5, 1.0, 0.7);
    CHECK(d.action == "B" && d.evaluations[0].blockedBy == "cooldown");
    d = step(9, 1.0, 0.7);  // A ended at 4, cooldown 5: available again at 9
    CHECK(d.action == "A" && d.switched);
    d = step(12, 0.0, 0.9);  // A's score drops to 0: no longer eligible, ends immediately despite commitment
    CHECK(d.action == "B" && d.reason == "switched");
    bb.set("enabled", BbValue(false));
    d = r.decide(bb, 13);  // B's prerequisite fails; A is cooling down (ended at 12)
    CHECK(d.action.empty() && d.reason == "none eligible" && d.switched);

    const std::string before = r.saveState().dump();
    CHECK_THROWS(r.decide(bb, 12));  // time went backwards
    CHECK_THROWS(r.decide(bb, kNan));
    CHECK(r.saveState().dump() == before);
    Blackboard foreign(std::make_shared<BlackboardSchema>(*s));
    CHECK_THROWS(r.decide(foreign, 20));
}

TEST("utility: model and per-agent state serialization") {
    auto s = combatSchema();
    UtilityAction A = attackAction();
    A.cooldownSeconds = 3;
    A.prerequisites = {obj({{"key", "enabled"}, {"op", "isTrue"}})};
    A.considerations[1].missing = Consideration::Missing::Value;
    A.considerations[1].missingValue = 0.25;
    UtilityAction idle;
    idle.name = "idle";
    idle.weight = 0.2;
    idle.priority = -1;
    const UtilityModel model(s, {A, idle}, 0.01, 0.2);
    const Json j = model.toJson();
    const UtilityModel back = UtilityModel::fromJson(Json::parse(j.dump()));
    CHECK(back.toJson() == j);
    Json v2 = j;
    v2["version"] = 2;
    CHECK_THROWS(UtilityModel::fromJson(v2));
    Json badMissing = j;
    badMissing["actions"].asArray()[0]["considerations"].asArray()[1]["missing"] = "guess";
    CHECK_THROWS(UtilityModel::fromJson(badMissing));

    auto shared = std::make_shared<UtilityModel>(model);
    UtilityReasoner r1(shared), r2(shared);
    Blackboard bb(s);
    bb.set("enemyVisible", BbValue(true));
    bb.set("distance", BbValue(5.0));
    r1.decide(bb, 0);
    bb.set("enemyVisible", BbValue(false));
    r1.decide(bb, 1);
    r2.loadState(Json::parse(r1.saveState().dump()));
    CHECK(r2.saveState() == r1.saveState() && r2.current() == r1.current());
    bb.set("enemyVisible", BbValue(true));
    for (double t : {2.0, 3.5, 4.0, 6.0}) CHECK(r1.decide(bb, t).action == r2.decide(bb, t).action);
    const std::string saved = r2.saveState().dump();
    Json unknown = r1.saveState();
    unknown["current"] = "dance";
    CHECK_THROWS(r2.loadState(unknown));
    Json backwards = r1.saveState();
    backwards["startedAt"] = 1e9;
    CHECK_THROWS(r2.loadState(backwards));
    CHECK(r2.saveState().dump() == saved);  // failed loads change nothing
}

TITAN_TEST_MAIN()
