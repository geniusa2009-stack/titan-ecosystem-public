// TITAN Engine — physics core (reference simulation). Original TITAN code.
#include "titan/physics.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace titan::physics {
namespace {

constexpr BodyId kMaxBodyId = (1ull << 53) - 1;
constexpr float kSlop = 0.005f;              // allowed resting penetration (m)
constexpr float kCorrection = 0.8f;          // fraction of excess penetration removed per step
constexpr float kRestitutionThreshold = 1.0f;  // closing speeds below this do not bounce (m/s)

void fail(const std::string& msg) { throw std::invalid_argument(msg); }

bool inRange(double v, double lo, double hi) { return v >= lo && v <= hi; }  // false for NaN

Vec3 clampLength(const Vec3& v, float max) {
    const float l = length(v);
    return l > max ? v * (max / l) : v;
}

float invMass(const Body& b) { return b.desc.type == BodyType::Dynamic ? 1.0f / b.desc.mass : 0.0f; }

bool awakeDynamic(const Body& b) { return b.desc.type == BodyType::Dynamic && !b.sleeping && !b.outOfBounds; }
bool activeKinematic(const Body& b) { return b.desc.type == BodyType::Kinematic && !b.outOfBounds; }

Quat integrate(const Quat& q, const Vec3& w, float dt) {
    const float a = length(w);
    if (!(a * dt > 1e-9f)) return q;
    return (Quat::axisAngle(w * (1.0f / a), a * dt) * q).normalized();
}

bool bodyFinite(const Body& b) {
    return finite(b.desc.position) && b.desc.rotation.finite() && finite(b.desc.linearVelocity) &&
           finite(b.desc.angularVelocity);
}

Placed placed(const BodyDesc& d) { return {d.shape, d.position, d.rotation}; }

Json vec(const Vec3& v) { return Json(Json::Array{Json(v.x), Json(v.y), Json(v.z)}); }
Vec3 vecFrom(const Json& j) {
    if (!j.isArray() || j.size() != 3) throw JsonError("expected [x, y, z]");
    return {float(j.at(0).asNumber()), float(j.at(1).asNumber()), float(j.at(2).asNumber())};
}

const char* typeName(BodyType t) {
    switch (t) {
        case BodyType::Static: return "static";
        case BodyType::Kinematic: return "kinematic";
        case BodyType::Dynamic: return "dynamic";
    }
    return "dynamic";
}
BodyType typeFrom(const std::string& s) {
    if (s == "static") return BodyType::Static;
    if (s == "kinematic") return BodyType::Kinematic;
    if (s == "dynamic") return BodyType::Dynamic;
    throw JsonError("unknown body type '" + s + "'");
}

Json shapeJson(const Shape& s) {
    Json o = Json::object();
    switch (s.kind) {
        case ShapeKind::Sphere: o["kind"] = "sphere"; o["radius"] = s.radius; break;
        case ShapeKind::Box: o["kind"] = "box"; o["halfExtents"] = vec(s.halfExtents); break;
        case ShapeKind::Capsule:
            o["kind"] = "capsule";
            o["radius"] = s.radius;
            o["halfHeight"] = s.halfHeight;
            break;
    }
    return o;
}
Shape shapeFrom(const Json& o) {
    if (!o.isObject()) throw JsonError("shape must be an object");
    const std::string kind = o["kind"].asString();
    if (kind == "sphere") return Shape::sphere(float(o["radius"].asNumber()));
    if (kind == "box") return Shape::box(vecFrom(o["halfExtents"]));
    if (kind == "capsule") return Shape::capsule(float(o["radius"].asNumber()), float(o["halfHeight"].asNumber()));
    throw JsonError("unknown shape kind '" + kind + "'");
}

void validateSettings(const Settings& s) {
    const Limits& l = s.limits;
    if (!finite(s.gravity) || length(s.gravity) > 1000) fail("gravity must be finite and at most 1000 m/s^2");
    if (!inRange(s.fixedDt, 1e-5, 0.1 + 1e-6)) fail("fixedDt must be in [1e-5, 0.1] seconds");
    if (s.maxSubsteps < 1 || s.maxSubsteps > 64) fail("maxSubsteps must be in [1, 64]");
    if (s.solverIterations < 1 || s.solverIterations > 64) fail("solverIterations must be in [1, 64]");
    if (!inRange(s.sleepLinearSpeed, 0, 1e3) || !inRange(s.sleepAngularSpeed, 0, 1e3) || !inRange(s.sleepTime, 0, 1e6))
        fail("sleep thresholds must be finite and non-negative");
    if (l.maxBodies < 1 || l.maxBodies > (1u << 24) || l.maxContacts < 1 || l.maxContacts > (1u << 24))
        fail("maxBodies and maxContacts must be in [1, 2^24]");
    if (!inRange(l.worldExtent, 1, 1e7)) fail("worldExtent must be in [1, 1e7] metres");
    if (!inRange(l.minDimension, 1e-6, 1e6) || !inRange(l.maxDimension, l.minDimension, 1e6))
        fail("dimension limits must satisfy 1e-6 <= minDimension <= maxDimension <= 1e6");
    if (!inRange(l.maxLinearSpeed, 1e-3, 1e5) || !inRange(l.maxAngularSpeed, 1e-3, 1e4)) fail("invalid speed limits");
}

}  // namespace

bool interacts(const BodyDesc& a, const BodyDesc& b) {
    return (a.mask & (1u << b.layer)) != 0 && (b.mask & (1u << a.layer)) != 0;
}

PhysicsWorld::PhysicsWorld(Settings settings) : settings_(std::move(settings)) { validateSettings(settings_); }

void PhysicsWorld::validateShape(const Shape& s) const {
    const Limits& l = settings_.limits;
    auto dim = [&](float v, const char* what) {
        if (!inRange(v, l.minDimension, l.maxDimension))
            fail(std::string("shape ") + what + " must be finite and within the dimension limits");
    };
    switch (s.kind) {
        case ShapeKind::Sphere: dim(s.radius, "radius"); return;
        case ShapeKind::Box:
            dim(s.halfExtents.x, "half extent");
            dim(s.halfExtents.y, "half extent");
            dim(s.halfExtents.z, "half extent");
            return;
        case ShapeKind::Capsule:
            dim(s.radius, "radius");
            if (!inRange(s.halfHeight, 0, l.maxDimension)) fail("capsule halfHeight must be finite and in [0, maxDimension]");
            return;
    }
    fail("unknown shape kind");
}

void PhysicsWorld::validatePlacement(const Vec3& p, const Quat& q) const {
    const float e = settings_.limits.worldExtent;
    if (!inRange(p.x, -e, e) || !inRange(p.y, -e, e) || !inRange(p.z, -e, e))
        fail("position must be finite and inside the world extent");
    if (!q.finite() || !inRange(q.norm(), 1 - 1e-3, 1 + 1e-3)) fail("rotation must be a finite unit quaternion");
}

void PhysicsWorld::validateDesc(const BodyDesc& d) const {
    if (d.type != BodyType::Static && d.type != BodyType::Kinematic && d.type != BodyType::Dynamic) fail("unknown body type");
    validateShape(d.shape);
    validatePlacement(d.position, d.rotation);
    const Limits& l = settings_.limits;
    if (!finite(d.linearVelocity) || !(length(d.linearVelocity) <= l.maxLinearSpeed)) fail("linear velocity exceeds the limit");
    if (!finite(d.angularVelocity) || !(length(d.angularVelocity) <= l.maxAngularSpeed))
        fail("angular velocity exceeds the limit");
    if (d.type == BodyType::Static && (!(d.linearVelocity == Vec3{}) || !(d.angularVelocity == Vec3{})))
        fail("static bodies cannot have a velocity");
    if (d.type == BodyType::Dynamic && !inRange(d.mass, 1e-6, 1e9)) fail("dynamic mass must be in [1e-6, 1e9]");
    if (!inRange(d.friction, 0, 10)) fail("friction must be in [0, 10]");
    if (!inRange(d.restitution, 0, 1)) fail("restitution must be in [0, 1]");
    if (!inRange(d.linearDamping, 0, 1000) || !inRange(d.angularDamping, 0, 1000)) fail("damping must be in [0, 1000]");
    if (!inRange(d.gravityScale, -100, 100)) fail("gravityScale must be in [-100, 100]");
    if (d.layer > 31) fail("layer must be in [0, 31]");
}

Body* PhysicsWorld::find(BodyId id) {
    auto it = std::lower_bound(bodies_.begin(), bodies_.end(), id, [](const Body& b, BodyId v) { return b.id < v; });
    return it != bodies_.end() && it->id == id ? &*it : nullptr;
}

const Body* PhysicsWorld::body(BodyId id) const { return const_cast<PhysicsWorld*>(this)->find(id); }

BodyId PhysicsWorld::addBody(const BodyDesc& desc) {
    if (bodies_.size() >= settings_.limits.maxBodies) throw std::length_error("physics body limit reached");
    if (nextId_ > kMaxBodyId) throw std::length_error("physics body ids exhausted");
    validateDesc(desc);
    Body b;
    b.desc = desc;
    b.desc.rotation = desc.rotation.normalized();
    b.id = nextId_;
    bodies_.push_back(b);  // ids only grow, so the vector stays sorted
    ++nextId_;
    return b.id;
}

bool PhysicsWorld::removeBody(BodyId id) {
    Body* b = find(id);
    if (!b) return false;
    bodies_.erase(bodies_.begin() + (b - bodies_.data()));
    return true;
}

void PhysicsWorld::setTransform(BodyId id, const Vec3& position, const Quat& rotation) {
    Body* b = find(id);
    if (!b) fail("unknown body " + std::to_string(id));
    validatePlacement(position, rotation);
    b->desc.position = position;
    b->desc.rotation = rotation.normalized();
    b->outOfBounds = false;
    wake(id);
}

void PhysicsWorld::setVelocity(BodyId id, const Vec3& linear, const Vec3& angular) {
    Body* b = find(id);
    if (!b) fail("unknown body " + std::to_string(id));
    if (b->desc.type == BodyType::Static) fail("static bodies cannot have a velocity");
    if (b->outOfBounds) fail("body " + std::to_string(id) + " is out of bounds; move it back first");
    const Limits& l = settings_.limits;
    if (!finite(linear) || !(length(linear) <= l.maxLinearSpeed)) fail("linear velocity exceeds the limit");
    if (!finite(angular) || !(length(angular) <= l.maxAngularSpeed)) fail("angular velocity exceeds the limit");
    b->desc.linearVelocity = linear;
    b->desc.angularVelocity = angular;
    wake(id);
}

void PhysicsWorld::applyImpulse(BodyId id, const Vec3& impulse) {
    Body* b = find(id);
    if (!b) fail("unknown body " + std::to_string(id));
    if (b->desc.type != BodyType::Dynamic) fail("impulses apply to dynamic bodies only");
    if (b->outOfBounds) fail("body " + std::to_string(id) + " is out of bounds; move it back first");
    if (!finite(impulse)) fail("impulse must be finite");
    const Vec3 v = b->desc.linearVelocity + impulse * invMass(*b);
    if (!finite(v) || !(length(v) <= settings_.limits.maxLinearSpeed)) fail("impulse exceeds the speed limit");
    b->desc.linearVelocity = v;
    wake(id);
}

void PhysicsWorld::setFilter(BodyId id, uint32_t layer, uint32_t mask) {
    Body* b = find(id);
    if (!b) fail("unknown body " + std::to_string(id));
    if (layer > 31) fail("layer must be in [0, 31]");
    b->desc.layer = layer;
    b->desc.mask = mask;
    wake(id);
}

void PhysicsWorld::wake(BodyId id) {
    Body* b = find(id);
    if (!b) fail("unknown body " + std::to_string(id));
    if (b->outOfBounds) return;
    b->sleeping = false;
    b->sleepTimer = 0;
}

void PhysicsWorld::step() {
    stats_ = {};
    simulate(settings_.fixedDt);
    stats_.steps = 1;
}

StepStats PhysicsWorld::advance(double frameDt) {
    if (!inRange(frameDt, 0, 1)) fail("frameDt must be finite and in [0, 1] seconds");
    stats_ = {};
    const double dt = settings_.fixedDt;
    accumulator_ += frameDt;
    while (accumulator_ >= dt && stats_.steps < settings_.maxSubsteps) {
        simulate(settings_.fixedDt);
        accumulator_ -= dt;
        ++stats_.steps;
    }
    if (accumulator_ >= dt) {
        const double keep = std::fmod(accumulator_, dt);
        stats_.droppedTime = accumulator_ - keep;
        accumulator_ = keep;
    }
    return stats_;
}

void PhysicsWorld::simulate(float dt) {
    snapshot_ = bodies_;
    contacts_.clear();
    triggerEvents_.clear();
    const Limits& lim = settings_.limits;

    // 1-2. Integrate velocities (dynamic) and positions (dynamic + kinematic).
    for (Body& b : bodies_) {
        BodyDesc& d = b.desc;
        if (awakeDynamic(b)) {
            d.linearVelocity = d.linearVelocity + settings_.gravity * (d.gravityScale * dt);
            d.linearVelocity = clampLength(d.linearVelocity * (1.0f / (1.0f + d.linearDamping * dt)), lim.maxLinearSpeed);
            d.angularVelocity =
                clampLength(d.angularVelocity * (1.0f / (1.0f + d.angularDamping * dt)), lim.maxAngularSpeed);
        }
        if (awakeDynamic(b) || activeKinematic(b)) {
            d.position = d.position + d.linearVelocity * dt;
            d.rotation = integrate(d.rotation, d.angularVelocity, dt);
        }
    }

    // 3. Broadphase: sort-and-sweep on x (stable: equal keys keep ascending id order).
    struct Entry {
        Aabb box;
        size_t index;
    };
    std::vector<Entry> entries;
    entries.reserve(bodies_.size());
    for (size_t i = 0; i < bodies_.size(); ++i)
        if (!bodies_[i].outOfBounds) entries.push_back({worldAabb(placed(bodies_[i].desc)), i});
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.box.min.x < b.box.min.x; });

    std::vector<std::pair<size_t, size_t>> pairIdx;  // body indices per contact
    std::set<std::pair<BodyId, BodyId>> triggerNow;
    for (size_t i = 0; i < entries.size(); ++i) {
        for (size_t j = i + 1; j < entries.size() && entries[j].box.min.x <= entries[i].box.max.x; ++j) {
            if (!entries[i].box.overlaps(entries[j].box)) continue;
            size_t ia = entries[i].index, ib = entries[j].index;
            if (bodies_[ia].id > bodies_[ib].id) std::swap(ia, ib);
            const Body& A = bodies_[ia];
            const Body& B = bodies_[ib];
            if (!interacts(A.desc, B.desc)) continue;
            const bool trigger = A.desc.trigger || B.desc.trigger;
            if (trigger) {
                if (A.desc.type == BodyType::Static && B.desc.type == BodyType::Static) continue;
            } else {
                const bool anyDynamic = A.desc.type == BodyType::Dynamic || B.desc.type == BodyType::Dynamic;
                const bool anyActive = awakeDynamic(A) || awakeDynamic(B) || activeKinematic(A) || activeKinematic(B);
                if (!anyDynamic || !anyActive) continue;
            }
            ++stats_.candidatePairs;
            const auto cp = collide(placed(A.desc), placed(B.desc));
            if (!cp) continue;
            if (trigger) {
                if (A.desc.trigger) triggerNow.emplace(A.id, B.id);
                if (B.desc.trigger) triggerNow.emplace(B.id, A.id);
                continue;
            }
            if (contacts_.size() >= lim.maxContacts) {
                ++stats_.skippedPairs;
                continue;
            }
            contacts_.push_back({A.id, B.id, *cp});
            pairIdx.emplace_back(ia, ib);
        }
    }
    // Deterministic contact order: by (a, b).
    {
        std::vector<size_t> order(contacts_.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t x, size_t y) {
            return std::make_pair(contacts_[x].a, contacts_[x].b) < std::make_pair(contacts_[y].a, contacts_[y].b);
        });
        std::vector<Contact> c2;
        std::vector<std::pair<size_t, size_t>> p2;
        c2.reserve(order.size());
        p2.reserve(order.size());
        for (size_t k : order) {
            c2.push_back(contacts_[k]);
            p2.push_back(pairIdx[k]);
        }
        contacts_.swap(c2);
        pairIdx.swap(p2);
    }

    // 4. Wake sleeping dynamic bodies touched by something moving.
    for (const auto& [ia, ib] : pairIdx) {
        Body& A = bodies_[ia];
        Body& B = bodies_[ib];
        const bool aMoving = awakeDynamic(A) || activeKinematic(A);
        const bool bMoving = awakeDynamic(B) || activeKinematic(B);
        if (A.sleeping && A.desc.type == BodyType::Dynamic && bMoving) { A.sleeping = false; A.sleepTimer = 0; }
        if (B.sleeping && B.desc.type == BodyType::Dynamic && aMoving) { B.sleeping = false; B.sleepTimer = 0; }
    }

    // 5. Sequential impulses (linear), then positional correction.
    struct Row {
        float bias = 0, mu = 0, jn = 0, jt = 0;
        Vec3 tangent;
        bool hasTangent = false;
    };
    std::vector<Row> rows(contacts_.size());
    for (size_t k = 0; k < contacts_.size(); ++k) {
        const BodyDesc& A = bodies_[pairIdx[k].first].desc;
        const BodyDesc& B = bodies_[pairIdx[k].second].desc;
        const Vec3& n = contacts_[k].geometry.normal;
        const Vec3 vrel = B.linearVelocity - A.linearVelocity;
        const float vn = dot(vrel, n);
        const float e = std::max(A.restitution, B.restitution);
        rows[k].bias = vn < -kRestitutionThreshold ? -e * vn : 0.0f;
        rows[k].mu = std::sqrt(A.friction * B.friction);
        const Vec3 vt = vrel - n * vn;
        const float lt = length(vt);
        if (lt > 1e-6f) {
            rows[k].tangent = vt * (1.0f / lt);
            rows[k].hasTangent = true;
        }
    }
    for (int it = 0; it < settings_.solverIterations; ++it) {
        for (size_t k = 0; k < contacts_.size(); ++k) {
            Body& A = bodies_[pairIdx[k].first];
            Body& B = bodies_[pairIdx[k].second];
            const float ia = invMass(A), ib = invMass(B), m = ia + ib;
            if (!(m > 0)) continue;
            Row& r = rows[k];
            const Vec3& n = contacts_[k].geometry.normal;
            float vn = dot(B.desc.linearVelocity - A.desc.linearVelocity, n);
            const float jn = std::max(r.jn + (r.bias - vn) / m, 0.0f);
            const float dj = jn - r.jn;
            r.jn = jn;
            A.desc.linearVelocity = A.desc.linearVelocity - n * (dj * ia);
            B.desc.linearVelocity = B.desc.linearVelocity + n * (dj * ib);
            if (r.hasTangent) {
                const float vt = dot(B.desc.linearVelocity - A.desc.linearVelocity, r.tangent);
                const float maxF = r.mu * r.jn;
                const float jt = std::clamp(r.jt - vt / m, -maxF, maxF);
                const float djt = jt - r.jt;
                r.jt = jt;
                A.desc.linearVelocity = A.desc.linearVelocity - r.tangent * (djt * ia);
                B.desc.linearVelocity = B.desc.linearVelocity + r.tangent * (djt * ib);
            }
        }
    }
    for (size_t k = 0; k < contacts_.size(); ++k) {
        Body& A = bodies_[pairIdx[k].first];
        Body& B = bodies_[pairIdx[k].second];
        const float ia = invMass(A), ib = invMass(B), m = ia + ib;
        const float excess = contacts_[k].geometry.depth - kSlop;
        if (!(m > 0) || !(excess > 0)) continue;
        const Vec3 corr = contacts_[k].geometry.normal * (excess * kCorrection / m);
        A.desc.position = A.desc.position - corr * ia;
        B.desc.position = B.desc.position + corr * ib;
    }
    for (Body& b : bodies_)
        if (b.desc.type == BodyType::Dynamic) b.desc.linearVelocity = clampLength(b.desc.linearVelocity, lim.maxLinearSpeed);

    // 6. Numerical guard: never leave non-finite state behind.
    for (const Body& b : bodies_) {
        if (!bodyFinite(b)) {
            bodies_.swap(snapshot_);
            contacts_.clear();
            throw std::runtime_error("physics step produced a non-finite state for body " + std::to_string(b.id) +
                                     "; the step was rolled back");
        }
    }

    // 7. Out-of-bounds freeze and sleeping.
    for (Body& b : bodies_) {
        if (b.desc.type == BodyType::Static || b.outOfBounds) continue;
        const Vec3& p = b.desc.position;
        const float e = lim.worldExtent;
        if (!inRange(p.x, -e, e) || !inRange(p.y, -e, e) || !inRange(p.z, -e, e)) {
            b.outOfBounds = true;
            b.sleeping = true;
            b.desc.linearVelocity = {};
            b.desc.angularVelocity = {};
            stats_.outOfBounds.push_back(b.id);
            continue;
        }
        if (!awakeDynamic(b)) continue;
        if (b.desc.canSleep && length(b.desc.linearVelocity) < settings_.sleepLinearSpeed &&
            length(b.desc.angularVelocity) < settings_.sleepAngularSpeed) {
            b.sleepTimer += dt;
            if (b.sleepTimer >= settings_.sleepTime) {
                b.sleeping = true;
                b.desc.linearVelocity = {};
                b.desc.angularVelocity = {};
            }
        } else {
            b.sleepTimer = 0;
        }
    }

    // 8. Trigger enter/exit events (exits first, each group ordered by (trigger, other)).
    for (const auto& p : triggerPairs_)
        if (!triggerNow.count(p)) triggerEvents_.push_back({TriggerEvent::Kind::Exit, p.first, p.second});
    for (const auto& p : triggerNow)
        if (!triggerPairs_.count(p)) triggerEvents_.push_back({TriggerEvent::Kind::Enter, p.first, p.second});
    triggerPairs_.swap(triggerNow);
}

std::optional<QueryHit> PhysicsWorld::raycast(const Vec3& origin, const Vec3& direction, float maxDistance,
                                              const QueryFilter& filter) const {
    if (!finite(origin)) fail("ray origin must be finite");
    const float len = length(direction);
    if (!finite(direction) || !(len > 1e-12f) || !std::isfinite(len)) fail("ray direction must be finite and non-zero");
    if (!inRange(maxDistance, 0, 1e9)) fail("ray maxDistance must be finite and in [0, 1e9]");
    const Vec3 d = direction * (1.0f / len);
    std::optional<QueryHit> best;
    for (const Body& b : bodies_) {
        if (!(filter.mask & (1u << b.desc.layer)) || (b.desc.trigger && !filter.includeTriggers)) continue;
        const float limit = best ? best->hit.distance : maxDistance;
        const auto h = physics::raycast(placed(b.desc), origin, d, limit);
        if (h && (!best || h->distance < best->hit.distance)) best = QueryHit{b.id, *h};
    }
    return best;
}

std::vector<BodyId> PhysicsWorld::overlap(const Shape& shape, const Vec3& position, const Quat& rotation,
                                          const QueryFilter& filter) const {
    validateShape(shape);
    validatePlacement(position, rotation);
    const Placed probe{shape, position, rotation.normalized()};
    const Aabb box = worldAabb(probe);
    std::vector<BodyId> out;
    for (const Body& b : bodies_) {
        if (!(filter.mask & (1u << b.desc.layer)) || (b.desc.trigger && !filter.includeTriggers)) continue;
        const Placed p = placed(b.desc);
        if (box.overlaps(worldAabb(p)) && collide(probe, p)) out.push_back(b.id);
    }
    return out;
}

Json PhysicsWorld::toJson() const {
    const Settings& s = settings_;
    Json st = Json::object();
    st["gravity"] = vec(s.gravity);
    st["fixedDt"] = s.fixedDt;
    st["maxSubsteps"] = s.maxSubsteps;
    st["solverIterations"] = s.solverIterations;
    st["sleepLinearSpeed"] = s.sleepLinearSpeed;
    st["sleepAngularSpeed"] = s.sleepAngularSpeed;
    st["sleepTime"] = s.sleepTime;
    Json lim = Json::object();
    lim["maxBodies"] = uint64_t(s.limits.maxBodies);
    lim["maxContacts"] = uint64_t(s.limits.maxContacts);
    lim["worldExtent"] = s.limits.worldExtent;
    lim["minDimension"] = s.limits.minDimension;
    lim["maxDimension"] = s.limits.maxDimension;
    lim["maxLinearSpeed"] = s.limits.maxLinearSpeed;
    lim["maxAngularSpeed"] = s.limits.maxAngularSpeed;
    st["limits"] = std::move(lim);

    Json bs = Json::array();
    for (const Body& b : bodies_) {
        const BodyDesc& d = b.desc;
        Json o = Json::object();
        o["id"] = b.id;
        o["type"] = typeName(d.type);
        o["shape"] = shapeJson(d.shape);
        o["position"] = vec(d.position);
        o["rotation"] = Json(Json::Array{Json(d.rotation.x), Json(d.rotation.y), Json(d.rotation.z), Json(d.rotation.w)});
        o["linearVelocity"] = vec(d.linearVelocity);
        o["angularVelocity"] = vec(d.angularVelocity);
        o["mass"] = d.mass;
        o["friction"] = d.friction;
        o["restitution"] = d.restitution;
        o["linearDamping"] = d.linearDamping;
        o["angularDamping"] = d.angularDamping;
        o["gravityScale"] = d.gravityScale;
        o["layer"] = d.layer;
        o["mask"] = d.mask;
        o["trigger"] = d.trigger;
        o["canSleep"] = d.canSleep;
        o["sleeping"] = b.sleeping;
        o["outOfBounds"] = b.outOfBounds;
        o["sleepTimer"] = b.sleepTimer;
        bs.push_back(std::move(o));
    }
    Json tp = Json::array();
    for (const auto& [t, other] : triggerPairs_) tp.push_back(Json(Json::Array{Json(t), Json(other)}));

    Json j = Json::object();
    j["format"] = "titan.physics";
    j["version"] = 1;
    j["settings"] = std::move(st);
    j["nextId"] = nextId_;
    j["accumulator"] = accumulator_;
    j["bodies"] = std::move(bs);
    j["triggerPairs"] = std::move(tp);
    return j;
}

PhysicsWorld PhysicsWorld::fromJson(const Json& j) {
    if (!j.isObject() || j.getString("format") != "titan.physics") throw JsonError("not a titan.physics document");
    const Json* version = j.find("version");
    if (!version || !version->isNumber() || version->asNumber() != 1) throw JsonError("unsupported titan.physics version");
    const Json& st = j["settings"];
    if (!st.isObject() || !st["limits"].isObject()) throw JsonError("titan.physics needs 'settings' with 'limits'");
    Settings s;
    s.gravity = vecFrom(st["gravity"]);
    s.fixedDt = float(st["fixedDt"].asNumber());
    s.maxSubsteps = int(std::clamp<int64_t>(st["maxSubsteps"].asInt(), -1, 1000));
    s.solverIterations = int(std::clamp<int64_t>(st["solverIterations"].asInt(), -1, 1000));
    s.sleepLinearSpeed = float(st["sleepLinearSpeed"].asNumber());
    s.sleepAngularSpeed = float(st["sleepAngularSpeed"].asNumber());
    s.sleepTime = float(st["sleepTime"].asNumber());
    const Json& lim = st["limits"];
    s.limits.maxBodies = size_t(std::min<uint64_t>(lim["maxBodies"].asUInt(), uint64_t(1) << 32));
    s.limits.maxContacts = size_t(std::min<uint64_t>(lim["maxContacts"].asUInt(), uint64_t(1) << 32));
    s.limits.worldExtent = float(lim["worldExtent"].asNumber());
    s.limits.minDimension = float(lim["minDimension"].asNumber());
    s.limits.maxDimension = float(lim["maxDimension"].asNumber());
    s.limits.maxLinearSpeed = float(lim["maxLinearSpeed"].asNumber());
    s.limits.maxAngularSpeed = float(lim["maxAngularSpeed"].asNumber());
    PhysicsWorld w(s);  // validates settings

    const Json& bs = j["bodies"];
    if (!bs.isArray()) throw JsonError("titan.physics needs a 'bodies' array");
    if (bs.size() > w.settings_.limits.maxBodies) fail("titan.physics has more bodies than its limit");
    BodyId last = 0;
    for (const Json& o : bs.asArray()) {
        if (!o.isObject()) throw JsonError("physics body must be an object");
        Body b;
        b.id = o["id"].asUInt();
        if (b.id == kNoBody || b.id <= last || b.id > kMaxBodyId) fail("physics body ids must be positive and ascending");
        last = b.id;
        BodyDesc& d = b.desc;
        d.type = typeFrom(o["type"].asString());
        d.shape = shapeFrom(o["shape"]);
        d.position = vecFrom(o["position"]);
        const Json& r = o["rotation"];
        if (!r.isArray() || r.size() != 4) throw JsonError("rotation must be [x, y, z, w]");
        d.rotation = {float(r.at(0).asNumber()), float(r.at(1).asNumber()), float(r.at(2).asNumber()), float(r.at(3).asNumber())};
        d.linearVelocity = vecFrom(o["linearVelocity"]);
        d.angularVelocity = vecFrom(o["angularVelocity"]);
        d.mass = float(o["mass"].asNumber());
        d.friction = float(o["friction"].asNumber());
        d.restitution = float(o["restitution"].asNumber());
        d.linearDamping = float(o["linearDamping"].asNumber());
        d.angularDamping = float(o["angularDamping"].asNumber());
        d.gravityScale = float(o["gravityScale"].asNumber());
        const uint64_t layer = o["layer"].asUInt(), mask = o["mask"].asUInt();
        if (layer > 31 || mask > kAllLayers) fail("invalid layer or mask");
        d.layer = uint32_t(layer);
        d.mask = uint32_t(mask);
        d.trigger = o["trigger"].asBool();
        d.canSleep = o["canSleep"].asBool();
        b.sleeping = o["sleeping"].asBool();
        b.outOfBounds = o["outOfBounds"].asBool();
        b.sleepTimer = float(o["sleepTimer"].asNumber());
        if (!inRange(b.sleepTimer, 0, 1e9)) fail("invalid sleepTimer");
        if (b.outOfBounds) {
            // Frozen bodies may sit just outside the extent: validate everything else at an in-bounds position.
            if (d.type == BodyType::Static || !finite(d.position)) fail("invalid out-of-bounds body");
            BodyDesc probe = d;
            probe.position = {};
            w.validateDesc(probe);
        } else {
            w.validateDesc(d);
        }
        w.bodies_.push_back(b);
    }
    w.nextId_ = j["nextId"].asUInt();
    if (w.nextId_ <= last || w.nextId_ > kMaxBodyId + 1) fail("nextId must be greater than every body id");
    w.accumulator_ = j["accumulator"].asNumber();
    if (!inRange(w.accumulator_, 0, w.settings_.fixedDt)) fail("invalid accumulator");
    const Json& tp = j["triggerPairs"];
    if (!tp.isArray()) throw JsonError("titan.physics needs a 'triggerPairs' array");
    for (const Json& p : tp.asArray()) {
        if (!p.isArray() || p.size() != 2) throw JsonError("trigger pair must be [trigger, other]");
        const BodyId t = p.at(0).asUInt(), other = p.at(1).asUInt();
        if (t == kNoBody || other == kNoBody || t == other) fail("invalid trigger pair");
        w.triggerPairs_.emplace(t, other);
    }
    return w;
}

}  // namespace titan::physics
