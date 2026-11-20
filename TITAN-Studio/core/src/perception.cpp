// TITAN Engine — AI perception data model. Original TITAN code.
#include "titan/perception.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace titan::ai {
namespace {

constexpr uint64_t kMaxId = (1ull << 53) - 1;
constexpr double kPi = 3.14159265358979323846;

void fail(const std::string& msg) { throw std::invalid_argument(msg); }
bool inRange(double v, double lo, double hi) { return v >= lo && v <= hi; }  // false for NaN
bool finite(const DVec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
double dot(const DVec3& a, const DVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
DVec3 sub(const DVec3& a, const DVec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double length(const DVec3& v) { return std::sqrt(dot(v, v)); }
void checkId(ActorId id) {
    if (id == 0 || id > kMaxId) fail("actor ids must be in [1, 2^53)");
}

spatial::Settings indexSettings(const PerceptionSettings& s) {
    spatial::Settings is;
    is.margin = 1.0;  // targets move often: absorb small moves without restructuring
    is.marginFraction = 0;
    is.maxProxies = std::max<size_t>(1, std::min<size_t>(s.maxTargets, size_t(1) << 30));
    is.maxCoordinate = s.maxCoordinate > 0 && s.maxCoordinate <= 1e15 ? s.maxCoordinate : 1e12;
    return is;
}

}  // namespace

// ---- config ----------------------------------------------------------------------------------------------------------

void PerceiverConfig::validate() const {
    if (!inRange(sightRadius, 0, 1e6)) fail("sightRadius must be in [0, 1e6]");
    if (!inRange(loseSightRadius, sightRadius, 1e6)) fail("loseSightRadius must be in [sightRadius, 1e6]");
    if (!(fovDegrees > 0) || !(fovDegrees <= 360)) fail("fovDegrees must be in (0, 360]");
    if (!inRange(hearingSensitivity, 0, 100)) fail("hearingSensitivity must be in [0, 100]");
    if (!inRange(memorySeconds, 0, 1e6)) fail("memorySeconds must be in [0, 1e6]");
    if (maxMemories < 1 || maxMemories > 65536) fail("maxMemories must be in [1, 65536]");
}

Json PerceiverConfig::toJson() const {
    Json j = Json::object();
    j["format"] = "titan.perception-config";
    j["version"] = 1;
    j["sightRadius"] = sightRadius;
    j["loseSightRadius"] = loseSightRadius;
    j["fovDegrees"] = fovDegrees;
    j["hearingSensitivity"] = hearingSensitivity;
    j["memorySeconds"] = memorySeconds;
    j["teamMask"] = teamMask;
    j["maxMemories"] = uint64_t(maxMemories);
    return j;
}

PerceiverConfig PerceiverConfig::fromJson(const Json& j) {
    if (!j.isObject() || j.getString("format") != "titan.perception-config")
        throw JsonError("not a titan.perception-config document");
    const Json* v = j.find("version");
    if (!v || !v->isNumber() || v->asNumber() != 1) throw JsonError("unsupported titan.perception-config version");
    PerceiverConfig c;
    c.sightRadius = j["sightRadius"].asNumber();
    c.loseSightRadius = j["loseSightRadius"].asNumber();
    c.fovDegrees = j["fovDegrees"].asNumber();
    c.hearingSensitivity = j["hearingSensitivity"].asNumber();
    c.memorySeconds = j["memorySeconds"].asNumber();
    const uint64_t mask = j["teamMask"].asUInt();
    if (mask > 0xFFFFFFFFull) fail("teamMask must fit 32 bits");
    c.teamMask = uint32_t(mask);
    c.maxMemories = size_t(std::min<uint64_t>(j["maxMemories"].asUInt(), uint64_t(1) << 32));
    c.validate();
    return c;
}

// ---- events ----------------------------------------------------------------------------------------------------------

const char* toString(PerceptionEvent::Kind k) {
    switch (k) {
        case PerceptionEvent::Kind::Removed: return "removed";
        case PerceptionEvent::Kind::SightLost: return "sightLost";
        case PerceptionEvent::Kind::Forgotten: return "forgotten";
        case PerceptionEvent::Kind::SightGained: return "sightGained";
        case PerceptionEvent::Kind::Heard: return "heard";
    }
    return "removed";
}

GameEvent toGameEvent(const PerceptionEvent& e) {
    GameEvent g;
    g.type = std::string("perception.") + toString(e.kind);
    g.source = e.perceiver;
    g.payload["target"] = e.target;
    g.payload["position"] = Json(Json::Array{Json(e.position.x), Json(e.position.y), Json(e.position.z)});
    g.payload["time"] = e.time;
    return g;
}

// ---- system ----------------------------------------------------------------------------------------------------------

PerceptionSystem::PerceptionSystem(PerceptionSettings s, std::shared_ptr<const LineOfSight> los)
    : settings_(s), los_(std::move(los)), index_(indexSettings(s)) {
    if (s.maxTargets < 1 || s.maxTargets > (size_t(1) << 24)) fail("maxTargets must be in [1, 2^24]");
    if (s.maxPerceivers < 1 || s.maxPerceivers > (size_t(1) << 20)) fail("maxPerceivers must be in [1, 2^20]");
    if (s.maxActiveNoises < 1 || s.maxActiveNoises > (size_t(1) << 20)) fail("maxActiveNoises must be in [1, 2^20]");
    if (!(s.maxCoordinate > 0) || !(s.maxCoordinate <= 1e12)) fail("maxCoordinate must be in (0, 1e12]");
}

void PerceptionSystem::checkPosition(const DVec3& p) const {
    const double e = settings_.maxCoordinate;
    if (!inRange(p.x, -e, e) || !inRange(p.y, -e, e) || !inRange(p.z, -e, e))
        fail("positions must be finite and within maxCoordinate");
}

void PerceptionSystem::addTarget(ActorId id, const DVec3& position, uint32_t team) {
    checkId(id);
    if (team > 31) fail("team must be in [0, 31]");
    checkPosition(position);
    if (targets_.count(id)) fail("target " + std::to_string(id) + " already exists");
    if (targets_.size() >= settings_.maxTargets) throw std::length_error("perception target limit reached");
    index_.insert(id, spatial::Bounds::around(position, {0, 0, 0}));
    try {
        targets_.emplace(id, Target{position, team});
    } catch (...) {
        index_.remove(id);
        throw;
    }
}

void PerceptionSystem::moveTarget(ActorId id, const DVec3& position) {
    auto it = targets_.find(id);
    if (it == targets_.end()) fail("unknown target " + std::to_string(id));
    checkPosition(position);
    index_.update(id, spatial::Bounds::around(position, {0, 0, 0}));
    it->second.position = position;
}

bool PerceptionSystem::removeTarget(ActorId id) {
    auto it = targets_.find(id);
    if (it == targets_.end()) return false;
    removed_.push_back(id);  // may throw before anything else changes
    index_.remove(id);
    targets_.erase(it);
    return true;
}

void PerceptionSystem::addPerceiver(ActorId id, const PerceiverConfig& config, const DVec3& position, const DVec3& forward) {
    checkId(id);
    config.validate();
    checkPosition(position);
    if (!finite(forward) || !(length(forward) > 1e-12)) fail("perceiver forward must be finite and non-zero");
    if (perceivers_.count(id)) fail("perceiver " + std::to_string(id) + " already exists");
    if (perceivers_.size() >= settings_.maxPerceivers) throw std::length_error("perceiver limit reached");
    Perceiver p;
    p.config = config;
    p.position = position;
    p.forward = forward;
    perceivers_.emplace(id, std::move(p));
}

void PerceptionSystem::setPerceiverPose(ActorId id, const DVec3& position, const DVec3& forward) {
    auto it = perceivers_.find(id);
    if (it == perceivers_.end()) fail("unknown perceiver " + std::to_string(id));
    checkPosition(position);
    if (!finite(forward) || !(length(forward) > 1e-12)) fail("perceiver forward must be finite and non-zero");
    it->second.position = position;
    it->second.forward = forward;
}

bool PerceptionSystem::removePerceiver(ActorId id) { return perceivers_.erase(id) != 0; }

void PerceptionSystem::emitNoise(const Noise& n) {
    if (n.source != 0) checkId(n.source);
    if (n.team > 31) fail("noise team must be in [0, 31]");
    checkPosition(n.position);
    if (!inRange(n.range, 0, 1e6)) fail("noise range must be in [0, 1e6]");
    if (!inRange(n.duration, 0, 3600)) fail("noise duration must be in [0, 3600] seconds");
    if (!std::isfinite(n.time)) fail("noise time must be finite");
    if (lastNow_ && n.time < *lastNow_) fail("noise time is before the last perception update");
    if (noises_.size() >= settings_.maxActiveNoises) throw std::length_error("too many active noises");
    noises_.push_back({nextNoise_++, n, false});
}

void PerceptionSystem::sense(ActorId id, Perceiver& p, double now, std::vector<PerceptionEvent>& out) {
    const PerceiverConfig& cfg = p.config;
    auto emit = [&](PerceptionEvent::Kind k, ActorId target, const DVec3& pos) {
        out.push_back({k, id, target, pos, now});
    };

    // Deleted actors: drop the memory and say so.
    for (ActorId rid : removed_) {
        auto it = p.memories.find(rid);
        if (it == p.memories.end()) continue;
        emit(PerceptionEvent::Kind::Removed, rid, it->second.lastKnownPosition);
        p.memories.erase(it);
    }

    // Sight.
    std::vector<spatial::Key> candidates;
    index_.querySphere(p.position, cfg.loseSightRadius, candidates);  // superset: exact tests follow
    const double fwdLen = length(p.forward);
    const DVec3 fwd{p.forward.x / fwdLen, p.forward.y / fwdLen, p.forward.z / fwdLen};
    const double cosHalf = std::cos(cfg.fovDegrees * 0.5 * kPi / 180.0);
    struct Seen {
        ActorId id;
        double distance;
    };
    std::vector<Seen> seen;
    for (spatial::Key k : candidates) {
        if (k == id) continue;
        const Target& t = targets_.at(k);
        if (!((cfg.teamMask >> t.team) & 1u)) continue;
        const DVec3 rel = sub(t.position, p.position);
        const double d = length(rel);
        auto mem = p.memories.find(k);
        const bool wasVisible = mem != p.memories.end() && mem->second.visible;
        if (d > (wasVisible ? cfg.loseSightRadius : cfg.sightRadius)) continue;
        if (cfg.fovDegrees < 360 && d > 1e-12 && dot(fwd, rel) / d < cosHalf - 1e-12) continue;
        if (los_ && !los_->clear(p.position, t.position, id, k)) continue;
        seen.push_back({k, d});
    }
    if (seen.size() > cfg.maxMemories) {  // keep the nearest
        std::sort(seen.begin(), seen.end(), [](const Seen& a, const Seen& b) {
            return a.distance != b.distance ? a.distance < b.distance : a.id < b.id;
        });
        seen.resize(cfg.maxMemories);
    }
    std::sort(seen.begin(), seen.end(), [](const Seen& a, const Seen& b) { return a.id < b.id; });
    std::set<ActorId> seenIds;
    for (const Seen& s : seen) {
        seenIds.insert(s.id);
        auto [it, inserted] = p.memories.try_emplace(s.id);
        Memory& m = it->second;
        if (inserted) m.firstSensedTime = now;
        const bool was = m.visible;
        m.visible = true;
        m.lastKnownPosition = targets_.at(s.id).position;
        m.lastSensedTime = now;
        m.lastSense = Sense::Sight;
        if (!was) emit(PerceptionEvent::Kind::SightGained, s.id, m.lastKnownPosition);
    }
    for (auto& [tid, m] : p.memories) {
        if (m.visible && !seenIds.count(tid)) {
            m.visible = false;
            emit(PerceptionEvent::Kind::SightLost, tid, m.lastKnownPosition);
        }
    }

    // Hearing.
    if (cfg.hearingSensitivity > 0) {
        for (const ActiveNoise& an : noises_) {
            const Noise& n = an.noise;
            if (n.time > now) continue;
            if (an.processed && now > n.time + n.duration) continue;
            if (n.source == id || p.heardNoises.count(an.id)) continue;
            if (!((cfg.teamMask >> n.team) & 1u)) continue;
            if (length(sub(n.position, p.position)) > n.range * cfg.hearingSensitivity) continue;
            p.heardNoises.insert(an.id);
            if (n.source != 0) {
                auto [it, inserted] = p.memories.try_emplace(n.source);
                Memory& m = it->second;
                if (inserted) m.firstSensedTime = now;
                if (!m.visible) {
                    m.lastKnownPosition = n.position;
                    m.lastSensedTime = now;
                    m.lastSense = Sense::Hearing;
                }
            }
            emit(PerceptionEvent::Kind::Heard, n.source, n.position);
        }
    }

    // Forgetting, then the memory cap (oldest unseen first, ties: lower id).
    for (auto it = p.memories.begin(); it != p.memories.end();) {
        if (!it->second.visible && now - it->second.lastSensedTime > cfg.memorySeconds) {
            emit(PerceptionEvent::Kind::Forgotten, it->first, it->second.lastKnownPosition);
            it = p.memories.erase(it);
        } else {
            ++it;
        }
    }
    while (p.memories.size() > cfg.maxMemories) {
        auto victim = p.memories.end();
        for (auto it = p.memories.begin(); it != p.memories.end(); ++it)
            if (!it->second.visible &&
                (victim == p.memories.end() || it->second.lastSensedTime < victim->second.lastSensedTime))
                victim = it;
        if (victim == p.memories.end()) break;  // cannot happen: visible entries are capped above
        emit(PerceptionEvent::Kind::Forgotten, victim->first, victim->second.lastKnownPosition);
        p.memories.erase(victim);
    }
}

const std::vector<PerceptionEvent>& PerceptionSystem::update(double now) {
    if (!std::isfinite(now)) fail("perception time must be finite");
    if (lastNow_ && now < *lastNow_) fail("perception time must not go backwards");
    std::sort(removed_.begin(), removed_.end());
    removed_.erase(std::unique(removed_.begin(), removed_.end()), removed_.end());
    events_.clear();
    for (auto& [id, p] : perceivers_) sense(id, p, now, events_);
    std::stable_sort(events_.begin(), events_.end(), [](const PerceptionEvent& a, const PerceptionEvent& b) {
        if (a.perceiver != b.perceiver) return a.perceiver < b.perceiver;
        if (a.target != b.target) return a.target < b.target;
        return int(a.kind) < int(b.kind);
    });
    // Noises: everything due has now had its chance; drop the expired ones and their bookkeeping.
    std::vector<uint64_t> dropped;
    for (ActiveNoise& n : noises_)
        if (n.noise.time <= now) n.processed = true;
    noises_.erase(std::remove_if(noises_.begin(), noises_.end(),
                                 [&](const ActiveNoise& n) {
                                     const bool gone = n.processed && now > n.noise.time + n.noise.duration;
                                     if (gone) dropped.push_back(n.id);
                                     return gone;
                                 }),
                  noises_.end());
    if (!dropped.empty())
        for (auto& [id, p] : perceivers_)
            for (uint64_t n : dropped) p.heardNoises.erase(n);
    removed_.clear();
    lastNow_ = now;
    return events_;
}

const std::map<ActorId, Memory>* PerceptionSystem::memories(ActorId perceiver) const {
    auto it = perceivers_.find(perceiver);
    return it == perceivers_.end() ? nullptr : &it->second.memories;
}

const Memory* PerceptionSystem::memory(ActorId perceiver, ActorId target) const {
    const auto* ms = memories(perceiver);
    if (!ms) return nullptr;
    auto it = ms->find(target);
    return it == ms->end() ? nullptr : &it->second;
}

std::optional<ActorId> PerceptionSystem::nearestVisible(ActorId perceiver) const {
    auto pit = perceivers_.find(perceiver);
    if (pit == perceivers_.end()) return std::nullopt;
    std::optional<ActorId> best;
    double bestD = 0;
    for (const auto& [tid, m] : pit->second.memories) {  // ascending id: strict < keeps the lower id on ties
        if (!m.visible) continue;
        const double d = length(sub(m.lastKnownPosition, pit->second.position));
        if (!best || d < bestD) {
            best = tid;
            bestD = d;
        }
    }
    return best;
}

}  // namespace titan::ai
