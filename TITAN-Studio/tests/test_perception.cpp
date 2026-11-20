// TITAN perception data model tests. Original TITAN code.
#include "titan/perception.h"
#include "titan_test.h"

#include <cmath>
#include <limits>

using namespace titan;
using namespace titan::ai;

namespace {
const double kNan = std::numeric_limits<double>::quiet_NaN();
using Kind = PerceptionEvent::Kind;

PerceiverConfig cfg(double sight = 10, double lose = 12, double fov = 90) {
    PerceiverConfig c;
    c.sightRadius = sight;
    c.loseSightRadius = lose;
    c.fovDegrees = fov;
    return c;
}

int count(const std::vector<PerceptionEvent>& es, Kind k, ActorId target = 0) {
    int n = 0;
    for (const auto& e : es)
        if (e.kind == k && (target == 0 || e.target == target)) ++n;
    return n;
}

struct Wall : LineOfSight {
    ActorId hidden;
    explicit Wall(ActorId h) : hidden(h) {}
    bool clear(const DVec3&, const DVec3&, ActorId, ActorId target) const override { return target != hidden; }
};

struct Rng {
    uint64_t s;
    double next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return double(s >> 11) * (1.0 / 9007199254740992.0);
    }
    double range(double a, double b) { return a + (b - a) * next(); }
};
}  // namespace

TEST("perception: invalid configuration, actors, noises and times are refused") {
    PerceiverConfig c = cfg();
    c.sightRadius = kNan;
    CHECK_THROWS(c.validate());
    c = cfg(10, 9);  // lose radius below sight radius
    CHECK_THROWS(c.validate());
    c = cfg(10, 12, 0);
    CHECK_THROWS(c.validate());
    c = cfg(10, 12, 400);
    CHECK_THROWS(c.validate());
    c = cfg();
    c.memorySeconds = -1;
    CHECK_THROWS(c.validate());
    c = cfg();
    c.maxMemories = 0;
    CHECK_THROWS(c.validate());
    const PerceiverConfig good = cfg(7.5, 9, 135);
    CHECK(PerceiverConfig::fromJson(Json::parse(good.toJson().dump())).toJson() == good.toJson());
    Json badMask = good.toJson();
    badMask["teamMask"] = 1e10;
    CHECK_THROWS(PerceiverConfig::fromJson(badMask));

    PerceptionSystem ps;
    CHECK_THROWS(ps.addTarget(0, {0, 0, 0}));
    CHECK_THROWS(ps.addTarget(1, {kNan, 0, 0}));
    CHECK_THROWS(ps.addTarget(1, {2e12, 0, 0}));
    CHECK_THROWS(ps.addTarget(1, {0, 0, 0}, 32));
    ps.addTarget(1, {0, 0, 0});
    CHECK_THROWS(ps.addTarget(1, {1, 0, 0}));
    CHECK_THROWS(ps.moveTarget(2, {0, 0, 0}));
    CHECK_THROWS(ps.moveTarget(1, {kNan, 0, 0}));
    CHECK_THROWS(ps.addPerceiver(5, cfg(), {0, 0, 0}, {0, 0, 0}));  // zero forward
    CHECK_THROWS(ps.addPerceiver(5, c, {0, 0, 0}, {0, 0, 1}));      // invalid config
    Noise n;
    n.range = kNan;
    CHECK_THROWS(ps.emitNoise(n));
    n = {};
    n.duration = 4000;
    CHECK_THROWS(ps.emitNoise(n));
    ps.update(5);
    n = {};
    n.time = 4;  // before the last update
    CHECK_THROWS(ps.emitNoise(n));
    CHECK_THROWS(ps.update(4));
    CHECK_THROWS(ps.update(kNan));
    PerceptionSettings tiny;
    tiny.maxActiveNoises = 1;
    PerceptionSystem few(tiny);
    few.emitNoise(Noise{});
    CHECK_THROWS(few.emitNoise(Noise{}));
}

TEST("perception: sight range and field-of-view boundaries, hysteresis and self") {
    PerceptionSystem ps;
    ps.addPerceiver(1, cfg(10, 12, 90), {0, 0, 0}, {0, 0, 1});
    ps.addTarget(1, {0, 0, 0});    // the perceiver's own body: never perceived
    ps.addTarget(2, {3, 0, 3});    // exactly on the 45 degree edge
    ps.addTarget(3, {3.01, 0, 3});  // just outside the cone
    ps.addTarget(4, {0, 0, -5});   // behind
    ps.addTarget(5, {0, 6, 8});    // distance exactly 10, inside the cone
    ps.addTarget(6, {0, 0, 10.001});
    const auto& ev = ps.update(0);
    CHECK(count(ev, Kind::SightGained) == 2);
    CHECK(count(ev, Kind::SightGained, 2) == 1 && count(ev, Kind::SightGained, 5) == 1);
    CHECK(!ps.memory(1, 1) && !ps.memory(1, 3) && !ps.memory(1, 4) && !ps.memory(1, 6));
    ps.moveTarget(5, {0, 0, 11.5});  // beyond sight but within lose radius: still seen
    CHECK(ps.update(1).empty() && ps.memory(1, 5)->visible);
    ps.moveTarget(5, {0, 0, 12.5});
    const auto& lost = ps.update(2);
    CHECK(count(lost, Kind::SightLost, 5) == 1 && !ps.memory(1, 5)->visible);
    CHECK(ps.memory(1, 5)->lastKnownPosition == (DVec3{0, 0, 11.5}));  // where it was last seen
    ps.moveTarget(5, {0, 0, 11});  // inside lose radius but outside sight radius: not re-acquired
    CHECK(ps.update(3).empty());
    ps.addPerceiver(2, cfg(10, 12, 360), {0, 0, 0}, {1, 0, 0});
    CHECK(count(ps.update(4), Kind::SightGained, 4) == 1);  // 360 degrees sees behind
}

TEST("perception: team filters and line-of-sight callbacks") {
    PerceptionSystem ps(PerceptionSettings{}, std::make_shared<Wall>(3));
    PerceiverConfig c = cfg(10, 10, 360);
    c.teamMask = ~(1u << 2);
    ps.addPerceiver(1, c, {0, 0, 0}, {0, 0, 1});
    ps.addTarget(2, {1, 0, 0}, 2);  // filtered team
    ps.addTarget(3, {2, 0, 0}, 0);  // behind a wall
    ps.addTarget(4, {3, 0, 0}, 1);
    const auto& ev = ps.update(0);
    CHECK(ev.size() == 1 && ev[0].target == 4 && ev[0].kind == Kind::SightGained);
    CHECK(ps.nearestVisible(1) == ActorId(4));
}

TEST("perception: hearing ranges, sensitivity, duplicates, own noises and expiry") {
    PerceptionSystem ps;
    PerceiverConfig deaf = cfg(0, 0, 360);
    PerceiverConfig keen = deaf;
    keen.hearingSensitivity = 2;
    ps.addPerceiver(1, deaf, {8, 0, 0}, {1, 0, 0});    // within 10
    ps.addPerceiver(2, deaf, {12, 0, 0}, {1, 0, 0});   // beyond 10
    ps.addPerceiver(3, keen, {12, 0, 0}, {1, 0, 0});   // 12 <= 10 * 2
    ps.addPerceiver(9, deaf, {0, 0, 0}, {1, 0, 0});    // the source itself
    Noise n;
    n.source = 9;
    n.range = 10;
    n.duration = 5;
    ps.emitNoise(n);
    const auto& ev = ps.update(0);
    CHECK(ev.size() == 2 && ev[0].perceiver == 1 && ev[1].perceiver == 3);
    CHECK(ev[0].kind == Kind::Heard && ev[0].target == 9 && ev[0].position == (DVec3{0, 0, 0}));
    const Memory* m = ps.memory(1, 9);
    CHECK(m && m->lastSense == Sense::Hearing && !m->visible);
    CHECK(ps.update(1).empty());  // each perceiver hears a noise once
    ps.setPerceiverPose(2, {5, 0, 0}, {1, 0, 0});
    CHECK(count(ps.update(2), Kind::Heard) == 1);  // walked into a still-active noise
    ps.setPerceiverPose(2, {12, 0, 0}, {1, 0, 0});
    Noise blip;
    blip.range = 100;
    blip.time = 3;
    blip.duration = 0;  // instantaneous, anonymous
    ps.emitNoise(blip);
    const auto& late = ps.update(7);  // first update after the blip: still heard exactly once
    CHECK(count(late, Kind::Heard) == 4 && count(late, Kind::Heard, 0) == 4);
    ps.addPerceiver(4, deaf, {1, 0, 0}, {1, 0, 0});
    CHECK(count(ps.update(8), Kind::Heard) == 0);  // both noises have expired
}

TEST("perception: memory expiry, caps and deleted actors") {
    PerceptionSystem ps;
    PerceiverConfig c = cfg(10, 10, 360);
    c.memorySeconds = 3;
    ps.addPerceiver(1, c, {0, 0, 0}, {0, 0, 1});
    ps.addTarget(2, {1, 0, 0});
    ps.addTarget(3, {2, 0, 0});
    CHECK(count(ps.update(0), Kind::SightGained) == 2);
    ps.moveTarget(2, {50, 0, 0});
    CHECK(count(ps.update(1), Kind::SightLost, 2) == 1);
    CHECK(ps.update(3).empty());  // last seen at t=0: remembered for 3 s
    const Memory* m = ps.memory(1, 2);
    CHECK(m && !m->visible && m->lastKnownPosition == (DVec3{1, 0, 0}) && m->lastSensedTime == 0);
    const auto& forget = ps.update(3.5);
    CHECK(forget.size() == 1 && forget[0].kind == Kind::Forgotten && forget[0].target == 2 && !ps.memory(1, 2));
    CHECK(ps.memory(1, 3)->visible);  // still seen: never forgotten

    PerceptionSystem capped;
    PerceiverConfig small = cfg(10, 10, 360);
    small.maxMemories = 2;
    capped.addPerceiver(1, small, {0, 0, 0}, {0, 0, 1});
    capped.addTarget(2, {1, 0, 0});
    capped.addTarget(3, {2, 0, 0});
    capped.addTarget(4, {3, 0, 0});
    capped.update(0);
    CHECK(capped.memory(1, 2) && capped.memory(1, 3) && !capped.memory(1, 4));  // the nearest are kept
    capped.moveTarget(2, {50, 0, 0});
    const auto& evicted = capped.update(1);  // 3 and 4 visible: the unseen memory of 2 makes room
    CHECK(count(evicted, Kind::SightLost, 2) == 1 && count(evicted, Kind::Forgotten, 2) == 1);
    CHECK(count(evicted, Kind::SightGained, 4) == 1 && !capped.memory(1, 2) && capped.memories(1)->size() == 2);

    PerceptionSystem del;
    del.addPerceiver(1, cfg(10, 10, 360), {0, 0, 0}, {0, 0, 1});
    del.addPerceiver(2, cfg(10, 10, 360), {0, 0, 1}, {0, 0, 1});
    del.addTarget(7, {1, 0, 0});
    del.update(0);
    CHECK(del.removeTarget(7) && !del.removeTarget(7) && !del.hasTarget(7));
    CHECK_THROWS(del.moveTarget(7, {0, 0, 0}));
    const auto& gone = del.update(1);
    CHECK(gone.size() == 2 && count(gone, Kind::Removed, 7) == 2 && count(gone, Kind::SightLost) == 0);
    CHECK(!del.memory(1, 7) && !del.memory(2, 7) && !del.nearestVisible(1));
    CHECK(del.update(20).empty());  // no late Forgotten for a removed actor
}

TEST("perception: indexed sight agrees with brute force and events are ordered deterministically") {
    struct T {
        DVec3 p;
        uint32_t team;
    };
    struct P {
        ActorId id;
        PerceiverConfig c;
        DVec3 pos, fwd;
    };
    std::map<ActorId, T> targets;
    std::vector<P> perceivers;
    Rng rng{2024};
    for (ActorId id = 1; id <= 400; ++id) {
        const double x = rng.range(-60, 60), y = rng.range(-5, 5), z = rng.range(-60, 60);
        targets[id] = {{1e6 + x, y, -1e6 + z}, uint32_t(id % 3)};
    }
    for (ActorId id = 1000; id < 1010; ++id) {
        P p;
        p.id = id;
        const double sight = rng.range(5, 30);
        const double fov = rng.range(30, 360);
        p.c = cfg(sight, 35, fov);
        p.c.teamMask = id % 2 ? 0x3u : 0x5u;
        const double px = rng.range(-40, 40), pz = rng.range(-40, 40);
        p.pos = {1e6 + px, 0, -1e6 + pz};
        const double fx = rng.range(-1, 1), fz = rng.range(0.1, 1);
        p.fwd = {fx, 0, fz};
        perceivers.push_back(p);
    }
    auto build = [&] {
        auto ps = std::make_unique<PerceptionSystem>();
        for (const auto& [id, t] : targets) ps->addTarget(id, t.p, t.team);
        for (const P& p : perceivers) ps->addPerceiver(p.id, p.c, p.pos, p.fwd);
        return ps;
    };
    auto ps = build();
    const std::vector<PerceptionEvent> ev = ps->update(0);
    size_t expected = 0;
    for (const P& p : perceivers) {
        const double fl = std::sqrt(p.fwd.x * p.fwd.x + p.fwd.y * p.fwd.y + p.fwd.z * p.fwd.z);
        const DVec3 f{p.fwd.x / fl, p.fwd.y / fl, p.fwd.z / fl};
        const double cosHalf = std::cos(p.c.fovDegrees * 0.5 * 3.14159265358979323846 / 180.0);
        std::vector<ActorId> want;
        for (const auto& [tid, t] : targets) {
            if (!((p.c.teamMask >> t.team) & 1u)) continue;
            const DVec3 r{t.p.x - p.pos.x, t.p.y - p.pos.y, t.p.z - p.pos.z};
            const double d = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
            if (d > p.c.sightRadius) continue;
            if (p.c.fovDegrees < 360 && d > 1e-12 && (f.x * r.x + f.y * r.y + f.z * r.z) / d < cosHalf - 1e-12) continue;
            want.push_back(tid);
        }
        std::vector<ActorId> got;
        for (const auto& e : ev)
            if (e.perceiver == p.id && e.kind == Kind::SightGained) got.push_back(e.target);
        CHECK(got == want);
        expected += want.size();
    }
    CHECK(ev.size() == expected && expected > 20);
    for (size_t i = 1; i < ev.size(); ++i)
        CHECK(ev[i - 1].perceiver < ev[i].perceiver ||
              (ev[i - 1].perceiver == ev[i].perceiver && ev[i - 1].target < ev[i].target));
    auto again = build();
    const std::vector<PerceptionEvent> ev2 = again->update(0);
    CHECK(ev2.size() == ev.size());
    for (size_t i = 0; i < ev.size(); ++i) CHECK(ev2[i].target == ev[i].target && ev2[i].perceiver == ev[i].perceiver);
}

TEST("perception: events convert to gameplay events") {
    PerceptionEvent e;
    e.kind = Kind::SightLost;
    e.perceiver = 12;
    e.target = 34;
    e.position = {1, 2, 3};
    e.time = 5.5;
    const GameEvent g = toGameEvent(e);
    CHECK(g.type == "perception.sightLost" && g.source == 12);
    CHECK(g.payload["target"].asUInt() == 34 && g.payload["time"].asNumber() == 5.5);
    CHECK(g.payload["position"].at(2).asNumber() == 3);
    CHECK(isValidEventType(g.type));
}

TITAN_TEST_MAIN()
