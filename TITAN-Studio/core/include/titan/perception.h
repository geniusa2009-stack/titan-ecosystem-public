// TITAN Engine — AI perception data model: sight and hearing stimuli, detection rules, memory of last-known
// observations and perception-change events. Original TITAN code.
//
// Scope and honesty. This is a backend-independent *model*. Sight is a range + field-of-view test plus an optional
// LineOfSight callback; without one, nothing is occluded (no realistic vision is claimed). Hearing is a range
// test against noise events (no acoustics, attenuation or occlusion). The Unreal adapter is expected to feed
// positions and to implement LineOfSight with real traces, and may map senses to UAIPerceptionComponent.
//
// Rules (PerceptionSystem::update, deterministic for the same inputs):
//  * Sight: a target is seen when it passes the perceiver's filter, is within `sightRadius` (or `loseSightRadius`
//    while already seen: hysteresis), within the field of view (angle to `forward` <= fov / 2; 360 sees all
//    around) and the LineOfSight callback (if any) agrees. Candidates come from the spatial index (titan/spatial.h).
//  * Hearing: a noise (emitted at `time`, lasting `duration`) is active while time <= now <= time + duration and
//    always during the first update at or after `time`, so short noises are never silently skipped. It is heard
//    once per perceiver when within noise.range * hearingSensitivity and its team passes the filter. Own noises
//    are ignored. Hearing refreshes the memory of the noise's source unless that source is currently seen.
//  * Memory: every sensed target keeps its last-known position, time and sense. Targets no longer sensed are
//    remembered for `memorySeconds`, then forgotten. At most `maxMemories` entries: the oldest unseen ones go first.
//  * Deleted actors: removeTarget() drops every perceiver's memory of that target on the next update with a
//    Removed event; no Lost / Forgotten events follow for it.
//  * Events are ordered by perceiver id, then target id, then kind (Removed, SightLost, Forgotten, SightGained,
//    Heard), and convert to gameplay events (titan/event_bus.h) with toGameEvent().
// Ids are in [1, 2^53). Time is caller-supplied seconds, finite and non-decreasing.
// Thread safety: single-threaded; the LineOfSight callback is invoked synchronously from update().
#pragma once

#include "titan/event_bus.h"
#include "titan/json.h"
#include "titan/spatial.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace titan::ai {

using spatial::DVec3;
using ActorId = uint64_t;

class LineOfSight {
public:
    virtual ~LineOfSight() = default;
    virtual bool clear(const DVec3& eye, const DVec3& target, ActorId perceiver, ActorId targetId) const = 0;
};

struct PerceiverConfig {
    double sightRadius = 20;        // [0, 1e6]
    double loseSightRadius = 25;    // [sightRadius, 1e6]
    double fovDegrees = 120;        // (0, 360]
    double hearingSensitivity = 1;  // [0, 100]; 0 disables hearing
    double memorySeconds = 10;      // [0, 1e6]
    uint32_t teamMask = 0xFFFFFFFFu;  // bit t set: actors of team t are perceived
    size_t maxMemories = 256;       // [1, 65536]

    void validate() const;  // std::invalid_argument
    Json toJson() const;    // {"format":"titan.perception-config","version":1,...}
    static PerceiverConfig fromJson(const Json& j);
};

struct Noise {
    ActorId source = 0;    // 0: anonymous / environment (no memory entry is created)
    uint32_t team = 0;     // 0..31
    DVec3 position;
    double range = 10;     // [0, 1e6]
    double time = 0;       // emission time
    double duration = 0;   // [0, 3600]; active while time <= now <= time + duration
};

enum class Sense { Sight, Hearing };

struct Memory {
    DVec3 lastKnownPosition;
    double lastSensedTime = 0;
    double firstSensedTime = 0;
    Sense lastSense = Sense::Sight;
    bool visible = false;  // currently seen
};

struct PerceptionEvent {
    enum class Kind { Removed, SightLost, Forgotten, SightGained, Heard };
    Kind kind = Kind::SightGained;
    ActorId perceiver = 0, target = 0;
    DVec3 position;  // last-known position
    double time = 0;
};
const char* toString(PerceptionEvent::Kind k);
GameEvent toGameEvent(const PerceptionEvent& e);  // type "perception.<kind>", source = perceiver

struct PerceptionSettings {
    size_t maxTargets = size_t(1) << 20;
    size_t maxPerceivers = 65536;
    size_t maxActiveNoises = 4096;
    double maxCoordinate = 1e12;
};

class PerceptionSystem {
public:
    explicit PerceptionSystem(PerceptionSettings settings = {}, std::shared_ptr<const LineOfSight> los = nullptr);

    // Actors that can be perceived. std::invalid_argument / std::length_error, nothing changes on failure.
    void addTarget(ActorId id, const DVec3& position, uint32_t team = 0);
    void moveTarget(ActorId id, const DVec3& position);
    bool removeTarget(ActorId id);
    bool hasTarget(ActorId id) const { return targets_.count(id) != 0; }

    void addPerceiver(ActorId id, const PerceiverConfig& config, const DVec3& position, const DVec3& forward);
    void setPerceiverPose(ActorId id, const DVec3& position, const DVec3& forward);
    bool removePerceiver(ActorId id);

    // std::length_error when maxActiveNoises are active; std::invalid_argument for invalid noises or a noise
    // emitted before the last update time.
    void emitNoise(const Noise& noise);

    // Runs every perceiver (ascending id) and returns this update's events. std::invalid_argument for a
    // non-finite or decreasing time (nothing changes).
    const std::vector<PerceptionEvent>& update(double now);

    const std::map<ActorId, Memory>* memories(ActorId perceiver) const;
    const Memory* memory(ActorId perceiver, ActorId target) const;
    std::optional<ActorId> nearestVisible(ActorId perceiver) const;  // ties: lower id

private:
    struct Target {
        DVec3 position;
        uint32_t team = 0;
    };
    struct Perceiver {
        PerceiverConfig config;
        DVec3 position, forward;
        std::map<ActorId, Memory> memories;
        std::set<uint64_t> heardNoises;
    };
    struct ActiveNoise {
        uint64_t id;
        Noise noise;
        bool processed = false;  // every noise gets at least one update, even with duration 0
    };
    void checkPosition(const DVec3& p) const;
    void sense(ActorId id, Perceiver& p, double now, std::vector<PerceptionEvent>& out);

    PerceptionSettings settings_;
    std::shared_ptr<const LineOfSight> los_;
    std::map<ActorId, Target> targets_;
    spatial::SpatialIndex index_;
    std::map<ActorId, Perceiver> perceivers_;
    std::vector<ActiveNoise> noises_;
    std::vector<ActorId> removed_;
    uint64_t nextNoise_ = 1;
    std::optional<double> lastNow_;
    std::vector<PerceptionEvent> events_;
};

}  // namespace titan::ai
