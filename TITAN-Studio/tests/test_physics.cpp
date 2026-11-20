// TITAN physics core tests. Original TITAN code.
#include "titan/physics.h"
#include "titan_test.h"

#include <limits>

using namespace titan;
using namespace titan::physics;

namespace {
const float kNan = std::numeric_limits<float>::quiet_NaN();
const float kInf = std::numeric_limits<float>::infinity();

Placed at(const Shape& s, Vec3 p, Quat q = {}) { return {s, p, q}; }
bool near(const Vec3& a, const Vec3& b, float eps = 1e-5f) { return length(a - b) <= eps; }

BodyDesc sphereBody(Vec3 p, float r = 0.5f, BodyType t = BodyType::Dynamic) {
    BodyDesc d;
    d.type = t;
    d.shape = Shape::sphere(r);
    d.position = p;
    return d;
}
BodyDesc boxBody(Vec3 p, Vec3 h, BodyType t = BodyType::Static) {
    BodyDesc d;
    d.type = t;
    d.shape = Shape::box(h);
    d.position = p;
    return d;
}
Settings noGravity() {
    Settings s;
    s.gravity = {0, 0, 0};
    return s;
}
}  // namespace

// ---- geometry -------------------------------------------------------------------------------------------------

TEST("physics geometry: sphere contacts, exact touching boundary and coincident centres") {
    const Shape s = Shape::sphere(0.5f);
    auto touch = collide(at(s, {0, 0, 0}), at(s, {1, 0, 0}));
    CHECK(touch && touch->depth == 0 && near(touch->normal, {1, 0, 0}));
    CHECK(!collide(at(s, {0, 0, 0}), at(s, {1.001f, 0, 0})));
    auto deep = collide(at(s, {0, 0, 0}), at(s, {0.8f, 0, 0}));
    CHECK(deep && std::fabs(deep->depth - 0.2f) < 1e-6f);
    auto same = collide(at(s, {2, 2, 2}), at(s, {2, 2, 2}));
    CHECK(same && near(same->normal, {0, 1, 0}) && same->depth == 1.0f);  // deterministic fallback normal
}

TEST("physics geometry: sphere-box faces, edges, inside centres and rotated boxes") {
    const Shape ball = Shape::sphere(0.5f);
    const Shape cube = Shape::box({1, 1, 1});
    auto face = collide(at(ball, {0, 1.5f, 0}), at(cube, {0, 0, 0}));
    CHECK(face && face->depth == 0 && near(face->normal, {0, -1, 0}));
    auto pen = collide(at(ball, {0, 1.4f, 0}), at(cube, {0, 0, 0}));
    CHECK(pen && std::fabs(pen->depth - 0.1f) < 1e-5f);
    auto edge = collide(at(ball, {1.3f, 1.3f, 0}), at(cube, {0, 0, 0}));
    CHECK(edge && near(edge->normal, normalize(Vec3{-1, -1, 0})) && std::fabs(edge->depth - (0.5f - std::sqrt(0.18f))) < 1e-5f);
    auto inside = collide(at(ball, {0, 0.9f, 0}), at(cube, {0, 0, 0}));
    CHECK(inside && near(inside->normal, {0, -1, 0}) && std::fabs(inside->depth - 0.6f) < 1e-5f);
    auto flipped = collide(at(cube, {0, 0, 0}), at(ball, {0, 1.4f, 0}));
    CHECK(flipped && near(flipped->normal, {0, 1, 0}));
    const Quat yaw45 = Quat::fromEulerDegrees({0, 45, 0});
    auto corner = collide(at(ball, {1.8f, 0, 0}), at(cube, {0, 0, 0}, yaw45));
    CHECK(corner && near(corner->normal, {-1, 0, 0}, 1e-4f));
    CHECK(std::fabs(corner->depth - (0.5f - (1.8f - std::sqrt(2.0f)))) < 1e-4f);
}

TEST("physics geometry: capsule contacts (parallel, crossing, against boxes)") {
    const Shape cap = Shape::capsule(0.5f, 1.0f);
    auto parallel = collide(at(cap, {0, 0, 0}), at(cap, {0.9f, 0.5f, 0}));
    CHECK(parallel && near(parallel->normal, {1, 0, 0}) && std::fabs(parallel->depth - 0.1f) < 1e-5f);
    const Quat lying = Quat::fromEulerDegrees({0, 0, 90});
    auto crossing = collide(at(cap, {0, 0, 0}), at(cap, {0, 0, 0.8f}, lying));
    CHECK(crossing && near(crossing->normal, {0, 0, 1}) && std::fabs(crossing->depth - 0.2f) < 1e-5f);
    const Shape cube = Shape::box({1, 1, 1});
    auto standing = collide(at(cap, {0, 2.4f, 0}), at(cube, {0, 0, 0}));
    CHECK(standing && near(standing->normal, {0, -1, 0}, 1e-4f) && std::fabs(standing->depth - 0.1f) < 1e-4f);
    auto flat = collide(at(cap, {0, 1.45f, 0}, lying), at(cube, {0, 0, 0}));
    CHECK(flat && near(flat->normal, {0, -1, 0}, 1e-4f) && std::fabs(flat->depth - 0.05f) < 1e-4f);
    CHECK(!collide(at(cap, {0, 2.6f, 0}), at(cube, {0, 0, 0})));
    auto degenerate = collide(at(Shape::capsule(0.5f, 0), {0, 0, 0}), at(Shape::sphere(0.5f), {0.5f, 0, 0}));
    CHECK(degenerate && std::fabs(degenerate->depth - 0.5f) < 1e-6f);  // zero-length capsule behaves as a sphere
}

TEST("physics geometry: box-box SAT finds face contacts and edge separations") {
    const Shape cube = Shape::box({1, 1, 1});
    auto stacked = collide(at(cube, {0, 0, 0}), at(cube, {0, 1.9f, 0}));
    CHECK(stacked && near(stacked->normal, {0, 1, 0}) && std::fabs(stacked->depth - 0.1f) < 1e-5f);
    auto touching = collide(at(cube, {0, 0, 0}), at(cube, {2, 0, 0}));
    CHECK(touching && touching->depth == 0);
    const Placed a = at(cube, {0, 0, 0});
    const Placed b = at(cube, {2.2f, 2.2f, 0}, Quat::fromEulerDegrees({0, 0, 45}));
    CHECK(worldAabb(a).overlaps(worldAabb(b)));  // bounding boxes overlap...
    CHECK(!collide(a, b));                        // ...but a separating axis exists
}

TEST("physics geometry: ray casts against all primitives with distance boundaries") {
    const Vec3 o{0, 0, 0}, z{0, 0, 1};
    auto s = raycast(at(Shape::sphere(1), {0, 0, 5}), o, z, 100);
    CHECK(s && s->distance == 4 && near(s->normal, {0, 0, -1}));
    CHECK(raycast(at(Shape::sphere(1), {0, 0, 5}), o, z, 4));
    CHECK(!raycast(at(Shape::sphere(1), {0, 0, 5}), o, z, 3.999f));
    auto b = raycast(at(Shape::box({1, 1, 1}), {0, 0, 5}), o, z, 100);
    CHECK(b && std::fabs(b->distance - 4) < 1e-6f && near(b->normal, {0, 0, -1}));
    auto inside = raycast(at(Shape::box({1, 1, 1}), {0, 0, 0}), o, z, 100);
    CHECK(inside && inside->distance == 0 && near(inside->normal, {0, 0, -1}));
    CHECK(!raycast(at(Shape::box({1, 1, 1}), {0, 0, 5}), {0, 2, 0}, z, 100));  // parallel, outside the slab
    CHECK(!raycast(at(Shape::sphere(1), {0, 0, -5}), o, z, 100));             // behind the origin
    const Shape cap = Shape::capsule(0.5f, 1);
    auto side = raycast(at(cap, {5, 0, 0}), o, {1, 0, 0}, 100);
    CHECK(side && std::fabs(side->distance - 4.5f) < 1e-5f && near(side->normal, {-1, 0, 0}));
    auto top = raycast(at(cap, {5, 0, 0}), {5, 5, 0}, {0, -1, 0}, 100);
    CHECK(top && std::fabs(top->distance - 3.5f) < 1e-5f && near(top->normal, {0, 1, 0}));
}

TEST("physics geometry: quaternions match the scene Euler convention") {
    const Vec3 euler{30, 45, 60};
    const Quat q = Quat::fromEulerDegrees(euler);
    Transform t;
    t.rotation = euler;
    const Mat4 m = t.matrix();
    for (const Vec3& v : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}, Vec3{0.3f, -2, 5}})
        CHECK(near(q.rotate(v), m.transformDirection(v), 1e-5f));
    CHECK(std::fabs(q.norm() - 1) < 1e-6f);
    const Quat bad{kNan, 0, 0, 1};
    CHECK(!bad.finite() && bad.normalized() == Quat{});
}

// ---- world: validation ----------------------------------------------------------------------------------------

TEST("physics world: invalid geometry, transforms and parameters are refused without side effects") {
    PhysicsWorld w;
    std::vector<BodyDesc> bad;
    auto with = [&](auto edit) {
        BodyDesc d = sphereBody({0, 0, 0});
        edit(d);
        bad.push_back(d);
    };
    with([](BodyDesc& d) { d.shape.radius = 0; });
    with([](BodyDesc& d) { d.shape.radius = -1; });
    with([](BodyDesc& d) { d.shape.radius = kNan; });
    with([](BodyDesc& d) { d.shape.radius = 1e5f; });
    with([](BodyDesc& d) { d.shape = Shape::box({1, kInf, 1}); });
    with([](BodyDesc& d) { d.shape = Shape::box({1, 0, 1}); });
    with([](BodyDesc& d) { d.shape = Shape::capsule(0.5f, -1); });
    with([](BodyDesc& d) { d.rotation = {0, 0, 0, 2}; });
    with([](BodyDesc& d) { d.rotation = {0, 0, 0, 0}; });
    with([](BodyDesc& d) { d.rotation = {kNan, 0, 0, 1}; });
    with([](BodyDesc& d) { d.position = {kNan, 0, 0}; });
    with([](BodyDesc& d) { d.position = {2e5f, 0, 0}; });
    with([](BodyDesc& d) { d.mass = 0; });
    with([](BodyDesc& d) { d.mass = kInf; });
    with([](BodyDesc& d) { d.type = BodyType::Static; d.linearVelocity = {1, 0, 0}; });
    with([](BodyDesc& d) { d.linearVelocity = {1e4f, 0, 0}; });
    with([](BodyDesc& d) { d.angularVelocity = {0, kNan, 0}; });
    with([](BodyDesc& d) { d.layer = 32; });
    with([](BodyDesc& d) { d.friction = -1; });
    with([](BodyDesc& d) { d.restitution = 1.5f; });
    with([](BodyDesc& d) { d.linearDamping = kNan; });
    with([](BodyDesc& d) { d.gravityScale = 1e3f; });
    for (const BodyDesc& d : bad) CHECK_THROWS(w.addBody(d));
    CHECK(w.size() == 0);
    const BodyId id = w.addBody(sphereBody({0, 0, 0}));
    CHECK(id == 1);  // failed additions consume no ids
    BodyDesc nearUnit = sphereBody({0, 0, 0});
    nearUnit.rotation = {0, 0, 0, 1.0005f};
    CHECK(std::fabs(w.body(w.addBody(nearUnit))->desc.rotation.w - 1) < 1e-7f);  // stored normalized
    const BodyDesc before = w.body(id)->desc;
    CHECK_THROWS(w.setTransform(id, {kNan, 0, 0}, {}));
    CHECK_THROWS(w.setVelocity(id, {0, 0, 0}, {kInf, 0, 0}));
    CHECK_THROWS(w.applyImpulse(id, {1e9f, 0, 0}));
    CHECK_THROWS(w.setFilter(id, 40, kAllLayers));
    CHECK_THROWS(w.setTransform(999, {}, {}));
    CHECK(w.body(id)->desc.position == before.position && w.body(id)->desc.linearVelocity == before.linearVelocity);
    Settings s;
    s.fixedDt = 0;
    CHECK_THROWS(PhysicsWorld(s));
    s = {};
    s.gravity = {0, kNan, 0};
    CHECK_THROWS(PhysicsWorld(s));
    s = {};
    s.limits.minDimension = 2;
    s.limits.maxDimension = 1;
    CHECK_THROWS(PhysicsWorld(s));
    Settings tiny;
    tiny.limits.maxBodies = 1;
    PhysicsWorld full(tiny);
    full.addBody(sphereBody({0, 0, 0}));
    CHECK_THROWS(full.addBody(sphereBody({0, 0, 0})));
}

// ---- world: simulation ----------------------------------------------------------------------------------------

TEST("physics world: collision filtering by layer and mask, static and kinematic pairs") {
    PhysicsWorld w(noGravity());
    BodyDesc a = sphereBody({0, 0, 0});
    a.layer = 1;
    a.mask = kAllLayers & ~(1u << 2);
    BodyDesc b = sphereBody({0.8f, 0, 0});
    b.layer = 2;
    CHECK(!interacts(a, b) && !interacts(b, a));
    const BodyId ia = w.addBody(a);
    w.addBody(b);
    w.addBody(boxBody({10, 0, 0}, {1, 1, 1}));
    w.addBody(boxBody({10.5f, 0, 0}, {1, 1, 1}));                       // static-static overlap
    w.addBody(boxBody({10, 3, 0}, {1, 1, 1}, BodyType::Kinematic));      // kinematic vs static
    w.addBody(boxBody({10, 3.5f, 0}, {1, 1, 1}));
    w.step();
    CHECK(w.contacts().empty());
    w.setFilter(ia, 1, kAllLayers);
    w.step();
    CHECK(w.contacts().size() == 1 && w.contacts()[0].a == ia && w.contacts()[0].a < w.contacts()[0].b);
}

TEST("physics world: triggers report enter and exit without contact response") {
    PhysicsWorld w(noGravity());
    BodyDesc zone = boxBody({0, 0, 0}, {1, 1, 1});
    zone.trigger = true;
    const BodyId tz = w.addBody(zone);
    BodyDesc ball = sphereBody({0, 3, 0});
    ball.linearVelocity = {0, -10, 0};
    ball.linearDamping = 0;
    ball.canSleep = false;
    const BodyId bb = w.addBody(ball);
    int enters = 0, exits = 0, enterStep = -1, exitStep = -1;
    for (int i = 0; i < 60; ++i) {
        w.step();
        CHECK(w.contacts().empty());
        for (const auto& e : w.triggerEvents()) {
            CHECK(e.trigger == tz && e.other == bb);
            if (e.kind == TriggerEvent::Kind::Enter) { ++enters; enterStep = i; }
            else { ++exits; exitStep = i; }
        }
    }
    CHECK(enters == 1 && exits == 1 && enterStep < exitStep);
    CHECK(w.body(bb)->desc.linearVelocity == Vec3(0, -10, 0));  // passed straight through
}

TEST("physics world: a dropped sphere comes to rest on the ground, sleeps and wakes on demand") {
    PhysicsWorld w;
    w.addBody(boxBody({0, -0.5f, 0}, {10, 0.5f, 10}));
    const BodyId ball = w.addBody(sphereBody({0, 3, 0}));
    for (int i = 0; i < 240; ++i) w.step();
    const Body* b = w.body(ball);
    CHECK(b->sleeping);
    CHECK(b->desc.position.y > 0.48f && b->desc.position.y <= 0.5005f);
    CHECK(b->desc.linearVelocity == Vec3{});
    const float restY = b->desc.position.y;
    w.step();
    CHECK(w.body(ball)->desc.position.y == restY);  // sleeping bodies do not drift
    w.applyImpulse(ball, {0, 5, 0});
    CHECK(!w.body(ball)->sleeping);
    w.step();
    CHECK(w.body(ball)->desc.position.y > restY);
    // A falling body wakes a sleeping one it lands on.
    for (int i = 0; i < 240; ++i) w.step();
    CHECK(w.body(ball)->sleeping);
    w.addBody(sphereBody({0, 3, 0}));
    bool woke = false;
    for (int i = 0; i < 120; ++i) {
        w.step();
        woke = woke || !w.body(ball)->sleeping;
    }
    CHECK(woke);
}

TEST("physics world: bouncing respects restitution and the restitution threshold") {
    PhysicsWorld w;
    BodyDesc ground = boxBody({0, -0.5f, 0}, {10, 0.5f, 10});
    ground.restitution = 1;
    w.addBody(ground);
    BodyDesc ball = sphereBody({0, 2, 0});
    ball.restitution = 1;
    ball.linearDamping = 0;
    const BodyId id = w.addBody(ball);
    bool rose = false;
    for (int i = 0; i < 60; ++i) {
        w.step();
        rose = rose || w.body(id)->desc.linearVelocity.y > 3;
    }
    CHECK(rose);
}

TEST("physics world: repeatable simulation and exact continuation after serialization") {
    auto build = [] {
        PhysicsWorld w;
        w.addBody(boxBody({0, -0.5f, 0}, {20, 0.5f, 20}));
        for (int i = 0; i < 18; ++i) {
            BodyDesc d = sphereBody({float(i % 6) * 0.9f - 2.0f, 1.0f + float(i / 6) * 1.1f, float(i % 3) * 0.3f});
            if (i % 3 == 1) d.shape = Shape::box({0.4f, 0.3f, 0.4f});
            if (i % 3 == 2) d.shape = Shape::capsule(0.3f, 0.4f);
            d.rotation = Quat::fromEulerDegrees({float(i) * 7, float(i) * 13, 0});
            d.angularVelocity = {0.1f * float(i % 4), 0, 0};
            d.restitution = 0.2f;
            d.friction = 0.6f;
            w.addBody(d);
        }
        BodyDesc k = boxBody({-6, 0.5f, 0}, {0.5f, 0.5f, 3}, BodyType::Kinematic);
        k.linearVelocity = {1, 0, 0};
        w.addBody(k);
        return w;
    };
    PhysicsWorld a = build(), b = build();
    for (int i = 0; i < 150; ++i) {
        a.step();
        b.step();
    }
    CHECK(a.toJson() == b.toJson());
    PhysicsWorld c = PhysicsWorld::fromJson(Json::parse(a.toJson().dump()));
    CHECK(c.toJson() == a.toJson());
    for (int i = 0; i < 150; ++i) {
        a.step();
        c.step();
        CHECK(a.contacts().size() == c.contacts().size());
    }
    CHECK(a.toJson() == c.toJson());
}

TEST("physics world: fixed-step accumulator, substep cap and invalid frame times") {
    Settings s = noGravity();
    s.fixedDt = 0.0625f;
    s.maxSubsteps = 4;
    PhysicsWorld w(s);
    CHECK(w.advance(0.25).steps == 4);
    CHECK(w.advance(0.1).steps == 1);
    CHECK(w.advance(0.03).steps == 1);  // 0.0375 carried + 0.03
    const StepStats long_ = w.advance(1.0);
    CHECK(long_.steps == 4);
    CHECK(std::fabs(long_.droppedTime - 0.75) < 1e-9);
    CHECK(w.advance(0).steps == 0);
    CHECK_THROWS(w.advance(std::numeric_limits<double>::quiet_NaN()));
    CHECK_THROWS(w.advance(-0.01));
    CHECK_THROWS(w.advance(2));
}

TEST("physics world: speed limits, out-of-bounds freezing and recovery") {
    Settings s;
    s.gravity = {0, -1000, 0};
    s.limits.maxLinearSpeed = 10;
    s.limits.worldExtent = 100;
    PhysicsWorld w(s);
    BodyDesc fast = sphereBody({0, 50, 0});
    fast.canSleep = false;
    const BodyId f = w.addBody(fast);
    for (int i = 0; i < 30; ++i) {
        w.step();
        CHECK(length(w.body(f)->desc.linearVelocity) <= 10.0001f);
    }
    BodyDesc runaway = sphereBody({99.9f, 0, 0});
    runaway.gravityScale = 0;
    runaway.linearVelocity = {10, 0, 0};
    const BodyId r = w.addBody(runaway);
    w.addBody(boxBody({99, -50, 0}, {0.5f, 0.5f, 0.5f}));  // static: never frozen
    w.step();
    CHECK(w.lastStats().outOfBounds.size() == 1 && w.lastStats().outOfBounds[0] == r);
    const Body* b = w.body(r);
    CHECK(b->outOfBounds && b->sleeping && b->desc.linearVelocity == Vec3{});
    const Vec3 frozenAt = b->desc.position;
    w.step();
    CHECK(w.body(r)->desc.position == frozenAt && w.lastStats().outOfBounds.empty());
    CHECK_THROWS(w.setVelocity(r, {1, 0, 0}, {}));
    CHECK_THROWS(w.applyImpulse(r, {1, 0, 0}));
    CHECK(PhysicsWorld::fromJson(w.toJson()).body(r)->outOfBounds);  // frozen bodies survive serialization
    w.setTransform(r, {0, 0, 0}, {});
    CHECK(!w.body(r)->outOfBounds && !w.body(r)->sleeping);
}

TEST("physics world: ray and overlap queries honour filters, triggers and ordering") {
    PhysicsWorld w(noGravity());
    const BodyId a = w.addBody(sphereBody({0, 0, 5}, 1, BodyType::Static));
    BodyDesc twin = sphereBody({0, 0, 5}, 1, BodyType::Static);
    twin.layer = 3;
    const BodyId b = w.addBody(twin);
    BodyDesc zone = boxBody({0, 0, 2}, {1, 1, 0.1f});
    zone.trigger = true;
    const BodyId t = w.addBody(zone);
    auto hit = w.raycast({0, 0, 0}, {0, 0, 2}, 100);  // direction need not be normalized
    CHECK(hit && hit->body == a && std::fabs(hit->hit.distance - 4) < 1e-5f);  // tie -> lowest id; trigger skipped
    QueryFilter onlyThree;
    onlyThree.mask = 1u << 3;
    CHECK(w.raycast({0, 0, 0}, {0, 0, 1}, 100, onlyThree)->body == b);
    QueryFilter withTriggers;
    withTriggers.includeTriggers = true;
    CHECK(w.raycast({0, 0, 0}, {0, 0, 1}, 100, withTriggers)->body == t);
    CHECK(!w.raycast({0, 0, 0}, {0, 0, 1}, 3.9f));
    CHECK_THROWS(w.raycast({kNan, 0, 0}, {0, 0, 1}, 1));
    CHECK_THROWS(w.raycast({0, 0, 0}, {0, 0, 0}, 1));
    CHECK_THROWS(w.raycast({0, 0, 0}, {0, 0, 1}, -1));

    PhysicsWorld q(noGravity());
    const BodyId touching = q.addBody(sphereBody({1.5f, 0, 0}, 0.5f, BodyType::Static));
    q.addBody(sphereBody({1.6f + 0.5f, 0, 0}, 0.5f, BodyType::Static));
    const BodyId inside = q.addBody(sphereBody({0, 0, 0}, 0.25f, BodyType::Static));
    const std::vector<BodyId> found = q.overlap(Shape::sphere(1), {0, 0, 0}, {});
    const std::vector<BodyId> want{touching, inside};
    CHECK(found == want);
    CHECK_THROWS(q.overlap(Shape::sphere(0), {0, 0, 0}, {}));
    CHECK_THROWS(q.overlap(Shape::sphere(1), {0, 0, 0}, {0, 0, 0, 0}));
}

TEST("physics world: serialization round trip and refusal of malformed documents") {
    PhysicsWorld w;
    w.addBody(boxBody({0, -1, 0}, {5, 1, 5}));
    BodyDesc cap = sphereBody({0, 2, 0});
    cap.shape = Shape::capsule(0.25f, 0.5f);
    cap.layer = 7;
    cap.mask = 0x80000001u;
    const BodyId last = w.addBody(cap);
    w.removeBody(last);
    const BodyId kept = w.addBody(cap);
    CHECK(kept == last + 1);  // ids are never reused
    const Json j = w.toJson();
    const PhysicsWorld back = PhysicsWorld::fromJson(Json::parse(j.dump()));
    CHECK(back.toJson() == j);
    CHECK(back.body(kept)->desc.mask == 0x80000001u);
    PhysicsWorld cont = PhysicsWorld::fromJson(j);
    CHECK(cont.addBody(cap) == kept + 1);
    Json v2 = j;
    v2["version"] = 2;
    CHECK_THROWS(PhysicsWorld::fromJson(v2));
    Json badRadius = j;
    badRadius["bodies"].asArray()[1]["shape"]["radius"] = -1;
    CHECK_THROWS(PhysicsWorld::fromJson(badRadius));
    Json unordered = j;
    std::swap(unordered["bodies"].asArray()[0], unordered["bodies"].asArray()[1]);
    CHECK_THROWS(PhysicsWorld::fromJson(unordered));
    Json staleNext = j;
    staleNext["nextId"] = 1;
    CHECK_THROWS(PhysicsWorld::fromJson(staleNext));
    Json badMask = j;
    badMask["bodies"].asArray()[1]["mask"] = 1e10;
    CHECK_THROWS(PhysicsWorld::fromJson(badMask));
}

TITAN_TEST_MAIN()
