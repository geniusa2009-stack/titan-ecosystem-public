// TITAN Engine — physics core: rigid-body model, collision filtering, fixed-step reference simulation and queries.
// Original TITAN code.
//
// Scope. This is TITAN's engine-independent physics *model*: body definitions, validation and limits, collision
// filtering, triggers, sleeping, query contracts and a versioned format. The Unreal integration maps it onto Chaos
// (BodyDesc -> FBodyInstance, layer/mask -> collision channels/responses, queries -> scene queries), which remains
// the production simulation backend there. The built-in solver below is a deliberately small *reference*
// implementation (semi-implicit Euler, linear sequential impulses with restitution and Coulomb friction, no
// rotational response to contacts, one contact per pair) used by headless tools and deterministic tests. It is not
// a Chaos replacement.
//
// Invariants (checked on every public mutation; violations throw std::invalid_argument and change nothing):
//  * Shapes: dimensions finite and within [limits.minDimension, limits.maxDimension] (capsule halfHeight may be 0).
//  * Positions finite with |coordinate| <= limits.worldExtent; rotations finite with |norm - 1| <= 1e-3 (stored
//    normalized); velocities finite and within the speed limits; dynamic mass finite in [1e-6, 1e9].
//  * Static bodies never move and have zero velocity. Kinematic bodies move only by their velocity / setTransform
//    and are never pushed. Dynamic bodies are simulated.
//  * Layers are 0..31; two bodies interact only if each one's mask contains the other's layer.
//  * Trigger bodies report enter/exit overlap events and never generate contact responses.
//
// Determinism: for the same sequence of calls the simulation is bitwise repeatable on the same build and platform:
// bodies are stored in ascending id order, pairs are generated and solved in a fixed order and no wall-clock
// time, randomness or unordered containers are involved.
//
// Failure model: step() snapshots body state and, if any body would become non-finite, restores the snapshot and
// throws std::runtime_error. A dynamic or kinematic body that leaves the world extent is frozen (asleep, zero
// velocity, outOfBounds = true) and reported in StepStats; setTransform back inside clears the flag.
//
// Cost: broadphase is sort-and-sweep on x, O(n log n + candidate pairs); contacts per step are capped at
// limits.maxContacts (excess pairs are skipped and counted). Queries are linear in the body count until the
// spatial index (titan/spatial) is wired in. Removing a body is O(n).
//
// Bodies removed while overlapping a trigger produce an Exit event (naming the removed id) on the next step.
// advance() commits each completed fixed step; if a later step throws, the earlier ones remain applied.
//
// Thread safety: none. A PhysicsWorld is owned and used by one thread; const queries may run concurrently with
// each other but not with any mutation.
// Not exposed over the protocol yet: no capability is advertised until it is wired and verified.
#pragma once

#include "titan/json.h"
#include "titan/physics_geometry.h"

#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace titan::physics {

using BodyId = uint64_t;  // 0 is "none"; never reused within a world
constexpr BodyId kNoBody = 0;
constexpr uint32_t kAllLayers = 0xFFFFFFFFu;

enum class BodyType { Static, Kinematic, Dynamic };

struct BodyDesc {
    BodyType type = BodyType::Dynamic;
    Shape shape;
    Vec3 position;
    Quat rotation;
    Vec3 linearVelocity;
    Vec3 angularVelocity;  // radians / second, world space
    float mass = 1.0f;     // dynamic only
    float friction = 0.5f;      // [0, 10]
    float restitution = 0.0f;   // [0, 1]
    float linearDamping = 0.01f;   // [0, 1000] per second
    float angularDamping = 0.05f;  // [0, 1000] per second
    float gravityScale = 1.0f;     // [-100, 100]
    uint32_t layer = 0;            // 0..31
    uint32_t mask = kAllLayers;
    bool trigger = false;
    bool canSleep = true;
};

struct Body {
    BodyId id = kNoBody;
    BodyDesc desc;
    bool sleeping = false;
    bool outOfBounds = false;
    float sleepTimer = 0;
};

struct Limits {
    size_t maxBodies = 65536;
    size_t maxContacts = 65536;
    float worldExtent = 1e5f;  // metres; single precision keeps ~1 cm resolution out to this distance
    float minDimension = 1e-3f;
    float maxDimension = 1e4f;
    float maxLinearSpeed = 500.0f;
    float maxAngularSpeed = 100.0f;
};

struct Settings {
    Vec3 gravity{0, -9.81f, 0};
    float fixedDt = 1.0f / 60.0f;  // [1e-5, 0.1]
    int maxSubsteps = 8;           // per advance(), [1, 64]
    int solverIterations = 8;      // [1, 64]
    float sleepLinearSpeed = 0.05f;
    float sleepAngularSpeed = 0.05f;
    float sleepTime = 0.5f;
    Limits limits;
};

struct Contact {
    BodyId a = kNoBody, b = kNoBody;  // a < b
    ContactPoint geometry;            // normal from a to b
};

struct TriggerEvent {
    enum class Kind { Enter, Exit };
    Kind kind = Kind::Enter;
    BodyId trigger = kNoBody;
    BodyId other = kNoBody;
};

struct StepStats {
    int steps = 0;
    double droppedTime = 0;  // advance() time discarded beyond maxSubsteps
    size_t candidatePairs = 0;
    size_t skippedPairs = 0;  // over limits.maxContacts
    std::vector<BodyId> outOfBounds;
};

struct QueryFilter {
    uint32_t mask = kAllLayers;
    bool includeTriggers = false;
};

struct QueryHit {
    BodyId body = kNoBody;
    RayHit hit;
};

class PhysicsWorld {
public:
    explicit PhysicsWorld(Settings settings = {});  // std::invalid_argument for invalid settings

    const Settings& settings() const { return settings_; }

    BodyId addBody(const BodyDesc& desc);
    bool removeBody(BodyId id);
    const Body* body(BodyId id) const;
    const std::vector<Body>& bodies() const { return bodies_; }  // ascending id
    size_t size() const { return bodies_.size(); }

    // Mutations validate first and wake the body (std::invalid_argument for unknown ids or invalid values).
    void setTransform(BodyId id, const Vec3& position, const Quat& rotation);
    void setVelocity(BodyId id, const Vec3& linear, const Vec3& angular);  // not for static bodies
    void applyImpulse(BodyId id, const Vec3& impulse);                    // dynamic bodies only
    void setFilter(BodyId id, uint32_t layer, uint32_t mask);
    void wake(BodyId id);

    // One fixed step of settings().fixedDt.
    void step();
    // Runs as many fixed steps as `frameDt` (plus carried remainder) allows, at most maxSubsteps; the rest of a
    // long frame is dropped and reported. frameDt must be finite and in [0, 1].
    StepStats advance(double frameDt);
    const StepStats& lastStats() const { return stats_; }

    // Results of the latest step, ordered by (a, b) / (trigger, other); trigger events: exits first, then enters.
    const std::vector<Contact>& contacts() const { return contacts_; }
    const std::vector<TriggerEvent>& triggerEvents() const { return triggerEvents_; }

    // Closest hit (ties: lowest id). direction need not be normalized but must be non-zero; maxDistance finite >= 0.
    std::optional<QueryHit> raycast(const Vec3& origin, const Vec3& direction, float maxDistance,
                                    const QueryFilter& filter = {}) const;
    // Bodies whose shapes touch or overlap the probe, ascending id. The probe shape is validated like a body's.
    std::vector<BodyId> overlap(const Shape& shape, const Vec3& position, const Quat& rotation,
                                const QueryFilter& filter = {}) const;

    // {"format":"titan.physics","version":1,"settings":{...},"nextId":n,"bodies":[...]}; fromJson validates all
    // input before constructing the world.
    Json toJson() const;
    static PhysicsWorld fromJson(const Json& j);

private:
    Body* find(BodyId id);
    void validateDesc(const BodyDesc& d) const;
    void validateShape(const Shape& s) const;
    void validatePlacement(const Vec3& p, const Quat& q) const;
    void simulate(float dt);

    Settings settings_;
    std::vector<Body> bodies_;
    BodyId nextId_ = 1;
    double accumulator_ = 0;
    StepStats stats_;
    std::vector<Contact> contacts_;
    std::vector<TriggerEvent> triggerEvents_;
    std::set<std::pair<BodyId, BodyId>> triggerPairs_;  // (trigger, other) overlapping after the latest step
    std::vector<Body> snapshot_;                         // reused rollback buffer
};

bool interacts(const BodyDesc& a, const BodyDesc& b);  // symmetric layer/mask test

}  // namespace titan::physics
