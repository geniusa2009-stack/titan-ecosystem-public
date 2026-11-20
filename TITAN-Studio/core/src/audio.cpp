// TITAN Engine — audio core. Original TITAN code.
#include "titan/audio.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace titan::audio {
namespace {

constexpr int kMaxBusDepth = 16;
constexpr float kMaxDistance = 1e6f;

void fail(const std::string& msg) { throw std::invalid_argument(msg); }
bool inRange(double v, double lo, double hi) { return v >= lo && v <= hi; }  // false for NaN
bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool inExtent(const Vec3& v, float e) { return inRange(v.x, -e, e) && inRange(v.y, -e, e) && inRange(v.z, -e, e); }

bool validName(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
               c == ':' || c == '/' || c == '-';
    });
}

void validateRef(const AssetRef& r) {
    if (r.path.empty() || r.path.size() > 512 || r.path.find('\0') != std::string::npos ||
        r.path.find('\\') != std::string::npos)
        fail("invalid audio asset path '" + r.path + "'");
    size_t start = 0;
    while (start <= r.path.size()) {
        const size_t end = r.path.find('/', start);
        const std::string seg = r.path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (seg == "..") fail("audio asset path must not contain '..': '" + r.path + "'");
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (r.revision < 1) fail("audio asset revision must be >= 1");
}

void validateInfo(const AssetInfo& i) {
    if (!(std::isfinite(i.duration) && i.duration > 0)) fail("audio asset duration must be finite and positive");
    if (i.channels < 1 || i.channels > 8) fail("audio asset channels must be in [1, 8]");
    if (i.sampleRate < 8000 || i.sampleRate > 192000) fail("audio asset sample rate must be in [8000, 192000]");
    if (i.revision < 1) fail("audio asset revision must be >= 1");
}

void validateAttenuation(const Attenuation& a) {
    using M = Attenuation::Model;
    switch (a.model) {
        case M::None: return;
        case M::Linear:
            if (!inRange(a.minDistance, 0, kMaxDistance) || !inRange(a.maxDistance, 0, kMaxDistance) ||
                !(a.minDistance < a.maxDistance))
                fail("linear attenuation needs 0 <= minDistance < maxDistance <= 1e6");
            return;
        case M::Inverse:
            if (!inRange(a.minDistance, 1e-6, kMaxDistance) || !inRange(a.maxDistance, 0, kMaxDistance) ||
                !(a.minDistance < a.maxDistance))
                fail("inverse attenuation needs 0 < minDistance < maxDistance <= 1e6");
            if (!inRange(a.rolloff, 1e-6, 100)) fail("inverse attenuation rolloff must be in (0, 100]");
            return;
        case M::Custom:
            if (a.points.size() < 2 || a.points.size() > 64) fail("custom attenuation needs 2 to 64 points");
            for (size_t i = 0; i < a.points.size(); ++i) {
                const auto& [d, g] = a.points[i];
                if (!inRange(d, 0, kMaxDistance) || !inRange(g, 0, 1)) fail("custom attenuation point out of range");
                if (i > 0 && !(d > a.points[i - 1].first)) fail("custom attenuation distances must strictly increase");
            }
            return;
    }
    fail("unknown attenuation model");
}

const char* modelName(Attenuation::Model m) {
    switch (m) {
        case Attenuation::Model::None: return "none";
        case Attenuation::Model::Linear: return "linear";
        case Attenuation::Model::Inverse: return "inverse";
        case Attenuation::Model::Custom: return "custom";
    }
    return "none";
}
Attenuation::Model modelFrom(const std::string& s) {
    if (s == "none") return Attenuation::Model::None;
    if (s == "linear") return Attenuation::Model::Linear;
    if (s == "inverse") return Attenuation::Model::Inverse;
    if (s == "custom") return Attenuation::Model::Custom;
    throw JsonError("unknown attenuation model '" + s + "'");
}
const char* policyName(ConcurrencyGroup::Policy p) {
    switch (p) {
        case ConcurrencyGroup::Policy::RejectNew: return "rejectNew";
        case ConcurrencyGroup::Policy::StopOldest: return "stopOldest";
        case ConcurrencyGroup::Policy::StopLowestPriority: return "stopLowestPriority";
    }
    return "stopOldest";
}
ConcurrencyGroup::Policy policyFrom(const std::string& s) {
    if (s == "rejectNew") return ConcurrencyGroup::Policy::RejectNew;
    if (s == "stopOldest") return ConcurrencyGroup::Policy::StopOldest;
    if (s == "stopLowestPriority") return ConcurrencyGroup::Policy::StopLowestPriority;
    throw JsonError("unknown concurrency policy '" + s + "'");
}

constexpr float kRadToDeg = 57.29577951308232f;

}  // namespace

// ---- assets ------------------------------------------------------------------------------------------------------

void MapAssetProvider::put(const std::string& path, const AssetInfo& info) {
    validateRef({path, info.revision});
    validateInfo(info);
    assets_[path] = info;
}

std::optional<AssetInfo> MapAssetProvider::find(const AssetRef& ref) const {
    auto it = assets_.find(ref.path);
    if (it == assets_.end() || it->second.revision < ref.revision) return std::nullopt;
    return it->second;
}

// ---- definitions -------------------------------------------------------------------------------------------------

float Attenuation::gain(float d) const {
    if (!(d >= 0)) d = 0;  // callers pass lengths; NaN never reaches the formulas
    switch (model) {
        case Model::None: return 1;
        case Model::Linear:
            if (d <= minDistance) return 1;
            if (d >= maxDistance) return 0;
            return 1 - (d - minDistance) / (maxDistance - minDistance);
        case Model::Inverse:
            if (d <= minDistance) return 1;
            if (d >= maxDistance) return 0;
            return minDistance / (minDistance + rolloff * (d - minDistance));
        case Model::Custom: {
            if (points.empty()) return 1;
            if (d <= points.front().first) return points.front().second;
            if (d >= points.back().first) return points.back().second;
            auto it = std::upper_bound(points.begin(), points.end(), d,
                                       [](float v, const std::pair<float, float>& p) { return v < p.first; });
            const auto& [d1, g1] = *it;
            const auto& [d0, g0] = *(it - 1);
            return g0 + (g1 - g0) * ((d - d0) / (d1 - d0));
        }
    }
    return 1;
}

AudioBank::AudioBank() {
    Bus master;
    master.name = "master";
    master.parent.clear();
    buses_.push_back(master);
    busIndex_["master"] = 0;
}

const Bus* AudioBank::bus(const std::string& n) const {
    auto it = busIndex_.find(n);
    return it == busIndex_.end() ? nullptr : &buses_[it->second];
}
const ConcurrencyGroup* AudioBank::group(const std::string& n) const {
    auto it = groupIndex_.find(n);
    return it == groupIndex_.end() ? nullptr : &groups_[it->second];
}
const SoundEvent* AudioBank::event(const std::string& n) const {
    auto it = eventIndex_.find(n);
    return it == eventIndex_.end() ? nullptr : &events_[it->second];
}

void AudioBank::addBus(const Bus& b) {
    if (!validName(b.name)) fail("invalid bus name '" + b.name + "'");
    if (bus(b.name)) fail("duplicate bus '" + b.name + "'");
    const Bus* parent = bus(b.parent);
    if (!parent) fail("bus '" + b.name + "': unknown parent '" + b.parent + "'");
    int depth = 1;
    for (const Bus* p = parent; !p->parent.empty(); p = bus(p->parent))
        if (++depth > kMaxBusDepth) fail("bus '" + b.name + "' is nested deeper than 16");
    if (!inRange(b.volume, 0, 4)) fail("bus '" + b.name + "': volume must be in [0, 4]");
    busIndex_[b.name] = buses_.size();
    buses_.push_back(b);
}

void AudioBank::addGroup(const ConcurrencyGroup& g) {
    if (!validName(g.name)) fail("invalid concurrency group name '" + g.name + "'");
    if (group(g.name)) fail("duplicate concurrency group '" + g.name + "'");
    if (g.maxVoices < 1 || g.maxVoices > 4096) fail("group '" + g.name + "': maxVoices must be in [1, 4096]");
    if (g.policy != ConcurrencyGroup::Policy::RejectNew && g.policy != ConcurrencyGroup::Policy::StopOldest &&
        g.policy != ConcurrencyGroup::Policy::StopLowestPriority)
        fail("group '" + g.name + "': unknown policy");
    groupIndex_[g.name] = groups_.size();
    groups_.push_back(g);
}

void AudioBank::addEvent(const SoundEvent& e) {
    if (!validName(e.name)) fail("invalid sound event name '" + e.name + "'");
    if (event(e.name)) fail("duplicate sound event '" + e.name + "'");
    const std::string where = "sound event '" + e.name + "': ";
    validateRef(e.asset);
    if (!bus(e.bus)) fail(where + "unknown bus '" + e.bus + "'");
    if (!e.group.empty() && !group(e.group)) fail(where + "unknown concurrency group '" + e.group + "'");
    if (!inRange(e.volume, 0, 4)) fail(where + "volume must be in [0, 4]");
    if (!inRange(e.pitch, 0.125, 8)) fail(where + "pitch must be in [0.125, 8]");
    if (!inRange(e.pan, -1, 1)) fail(where + "pan must be in [-1, 1]");
    if (!inRange(e.spread, 0, 1)) fail(where + "spread must be in [0, 1]");
    if (e.priority < 0 || e.priority > 255) fail(where + "priority must be in [0, 255]");
    validateAttenuation(e.attenuation);
    eventIndex_[e.name] = events_.size();
    events_.push_back(e);
}

Json AudioBank::toJson() const {
    Json j = Json::object();
    j["format"] = "titan.audio";
    j["version"] = 1;
    Json bs = Json::array();
    for (const Bus& b : buses_) {
        Json o = Json::object();
        o["name"] = b.name;
        o["parent"] = b.parent;
        o["volume"] = b.volume;
        o["muted"] = b.muted;
        bs.push_back(std::move(o));
    }
    j["buses"] = std::move(bs);
    Json gs = Json::array();
    for (const ConcurrencyGroup& g : groups_) {
        Json o = Json::object();
        o["name"] = g.name;
        o["maxVoices"] = g.maxVoices;
        o["policy"] = policyName(g.policy);
        gs.push_back(std::move(o));
    }
    j["groups"] = std::move(gs);
    Json es = Json::array();
    for (const SoundEvent& e : events_) {
        Json o = Json::object();
        o["name"] = e.name;
        Json a = Json::object();
        a["path"] = e.asset.path;
        a["revision"] = e.asset.revision;
        o["asset"] = std::move(a);
        o["bus"] = e.bus;
        if (!e.group.empty()) o["group"] = e.group;
        o["volume"] = e.volume;
        o["pitch"] = e.pitch;
        o["looping"] = e.looping;
        o["spatial"] = e.spatial;
        o["pan"] = e.pan;
        o["spread"] = e.spread;
        o["priority"] = e.priority;
        Json at = Json::object();
        at["model"] = modelName(e.attenuation.model);
        at["minDistance"] = e.attenuation.minDistance;
        at["maxDistance"] = e.attenuation.maxDistance;
        at["rolloff"] = e.attenuation.rolloff;
        Json pts = Json::array();
        for (const auto& [d, g] : e.attenuation.points) pts.push_back(Json(Json::Array{Json(d), Json(g)}));
        at["points"] = std::move(pts);
        o["attenuation"] = std::move(at);
        es.push_back(std::move(o));
    }
    j["events"] = std::move(es);
    return j;
}

AudioBank AudioBank::fromJson(const Json& j) {
    if (!j.isObject() || j.getString("format") != "titan.audio") throw JsonError("not a titan.audio document");
    const Json* version = j.find("version");
    if (!version || !version->isNumber() || version->asNumber() != 1) throw JsonError("unsupported titan.audio version");
    if (!j["buses"].isArray() || !j["groups"].isArray() || !j["events"].isArray())
        throw JsonError("titan.audio needs 'buses', 'groups' and 'events' arrays");
    AudioBank bank;
    for (const Json& o : j["buses"].asArray()) {
        if (!o.isObject()) throw JsonError("bus must be an object");
        Bus b;
        b.name = o["name"].asString();
        b.parent = o.getString("parent");
        b.volume = float(o.getNumber("volume", 1));
        b.muted = o.getBool("muted", false);
        if (b.name == "master") {
            if (!b.parent.empty() || !inRange(b.volume, 0, 4)) fail("invalid master bus");
            bank.buses_[0] = b;
        } else {
            bank.addBus(b);
        }
    }
    for (const Json& o : j["groups"].asArray()) {
        if (!o.isObject()) throw JsonError("concurrency group must be an object");
        ConcurrencyGroup g;
        g.name = o["name"].asString();
        g.maxVoices = int(std::clamp<int64_t>(o["maxVoices"].asInt(), -1, 1 << 20));
        g.policy = policyFrom(o.getString("policy", "stopOldest"));
        bank.addGroup(g);
    }
    for (const Json& o : j["events"].asArray()) {
        if (!o.isObject() || !o["asset"].isObject() || !o["attenuation"].isObject())
            throw JsonError("sound event must be an object with 'asset' and 'attenuation'");
        SoundEvent e;
        e.name = o["name"].asString();
        e.asset.path = o["asset"]["path"].asString();
        e.asset.revision = uint32_t(std::min<uint64_t>(o["asset"]["revision"].asUInt(), 0xFFFFFFFFu));
        e.bus = o.getString("bus", "master");
        e.group = o.getString("group");
        e.volume = float(o.getNumber("volume", 1));
        e.pitch = float(o.getNumber("pitch", 1));
        e.looping = o.getBool("looping", false);
        e.spatial = o.getBool("spatial", true);
        e.pan = float(o.getNumber("pan", 0));
        e.spread = float(o.getNumber("spread", 0));
        e.priority = int(std::clamp<int64_t>(o["priority"].asInt(), -1, 1000));
        const Json& at = o["attenuation"];
        e.attenuation.model = modelFrom(at["model"].asString());
        e.attenuation.minDistance = float(at.getNumber("minDistance", 1));
        e.attenuation.maxDistance = float(at.getNumber("maxDistance", 100));
        e.attenuation.rolloff = float(at.getNumber("rolloff", 1));
        if (const Json* pts = at.find("points")) {
            if (!pts->isArray()) throw JsonError("attenuation points must be an array");
            for (const Json& p : pts->asArray()) {
                if (!p.isArray() || p.size() != 2) throw JsonError("attenuation point must be [distance, gain]");
                e.attenuation.points.emplace_back(float(p.at(0).asNumber()), float(p.at(1).asNumber()));
            }
        }
        bank.addEvent(e);
    }
    return bank;
}

// ---- runtime -----------------------------------------------------------------------------------------------------

AudioSystem::AudioSystem(AudioBank bank, std::unique_ptr<AudioBackend> backend,
                         std::shared_ptr<const AssetProvider> assets, AudioConfig config)
    : bank_(std::move(bank)), backend_(std::move(backend)), assets_(std::move(assets)), config_(config) {
    if (!backend_) fail("audio backend is required");
    if (!assets_) fail("audio asset provider is required");
    if (config_.maxVoices < 1 || config_.maxVoices > 65536) fail("maxVoices must be in [1, 65536]");
    if (config_.maxRealVoices < 0 || config_.maxRealVoices > config_.maxVoices)
        fail("maxRealVoices must be in [0, maxVoices]");
    if (!inRange(config_.worldExtent, 1, 1e7)) fail("worldExtent must be in [1, 1e7]");
    for (const Bus& b : bank_.buses()) busState_[b.name] = b;
    voices_.reserve(size_t(config_.maxVoices));
}

AudioSystem::~AudioSystem() {
    try {
        stopAll();
    } catch (...) {
        // A backend must not throw from stopVoice; never let it escape a destructor.
    }
}

VoiceId AudioSystem::idOf(size_t slot) const { return (uint64_t(voices_[slot].generation) << 32) | uint64_t(slot + 1); }

const AudioSystem::Voice* AudioSystem::resolve(VoiceId id) const {
    const uint64_t low = id & 0xFFFFFFFFull;
    if (low == 0 || low > voices_.size()) return nullptr;
    const Voice& v = voices_[size_t(low - 1)];
    return v.active && v.generation == uint32_t(id >> 32) ? &v : nullptr;
}
AudioSystem::Voice* AudioSystem::resolve(VoiceId id) {
    return const_cast<Voice*>(static_cast<const AudioSystem*>(this)->resolve(id));
}

void AudioSystem::release(size_t slot) {
    Voice& v = voices_[slot];
    const VoiceId id = idOf(slot);
    const bool wasReal = v.real;
    v.active = false;
    v.real = false;
    v.event = nullptr;
    v.generation = v.generation == 0xFFFFFFFFu ? 1 : v.generation + 1;
    --active_;
    free_.push_back(slot);
    if (wasReal) backend_->stopVoice(id);
}

RenderParams AudioSystem::compute(const Voice& v, const std::map<std::string, float>& busGains) const {
    const SoundEvent& e = *v.event;
    RenderParams p;
    p.pitch = e.pitch * v.pitch;
    p.looping = e.looping;
    p.playbackTime = v.time;
    auto bg = busGains.find(e.bus);
    float gain = e.volume * v.volume * listener_.gain * (bg == busGains.end() ? 1.0f : bg->second);
    if (e.spatial) {
        const Vec3 rel = v.position - listener_.position;
        const float dist = length(rel);
        p.distance = dist;
        gain *= e.attenuation.gain(dist);
        if (dist > 1e-6f) {
            const Vec3 f = normalize(listener_.forward);
            const Vec3 r = normalize(cross(f, listener_.up));
            const Vec3 u = cross(r, f);
            const Vec3 dir = rel * (1.0f / dist);
            const float x = dot(dir, r), y = dot(dir, u), z = dot(dir, f);
            p.pan = std::clamp(x * (1 - e.spread), -1.0f, 1.0f);
            p.azimuth = std::atan2(x, z) * kRadToDeg;
            p.elevation = std::asin(std::clamp(y, -1.0f, 1.0f)) * kRadToDeg;
        }
    } else {
        p.pan = e.pan;
    }
    p.gain = gain;
    return p;
}

PlayResult AudioSystem::play(const std::string& name, const PlayParams& params) {
    const SoundEvent* e = bank_.event(name);
    if (!e) return {PlayStatus::UnknownEvent, 0};
    if (!finite(params.position) || !inExtent(params.position, config_.worldExtent) || !inRange(params.volume, 0, 4) ||
        !inRange(params.pitch, 0.125, 8))
        return {PlayStatus::InvalidParams, 0};
    const std::optional<AssetInfo> info = assets_->find(e->asset);
    if (!info || !(std::isfinite(info->duration) && info->duration > 0)) {
        ++stats_.missingAssets;
        return {PlayStatus::MissingAsset, 0};
    }

    // Decide every eviction before changing anything.
    auto oldestLowest = [&](auto&& include, int belowOrEqual, bool strict) -> std::optional<size_t> {
        std::optional<size_t> victim;
        for (size_t i = 0; i < voices_.size(); ++i) {
            const Voice& v = voices_[i];
            if (!v.active || !include(v)) continue;
            const int pr = v.event->priority;
            if (strict ? !(pr < belowOrEqual) : !(pr <= belowOrEqual)) continue;
            if (!victim) { victim = i; continue; }
            const Voice& cur = voices_[*victim];
            if (pr < cur.event->priority || (pr == cur.event->priority && v.order < cur.order)) victim = i;
        }
        return victim;
    };
    std::optional<size_t> evict;
    if (!e->group.empty()) {
        const ConcurrencyGroup& g = *bank_.group(e->group);
        int inGroup = 0;
        std::optional<size_t> oldest;
        for (size_t i = 0; i < voices_.size(); ++i) {
            const Voice& v = voices_[i];
            if (!v.active || v.event->group != e->group) continue;
            ++inGroup;
            if (!oldest || v.order < voices_[*oldest].order) oldest = i;
        }
        if (inGroup >= g.maxVoices) {
            switch (g.policy) {
                case ConcurrencyGroup::Policy::RejectNew: break;
                case ConcurrencyGroup::Policy::StopOldest: evict = oldest; break;
                case ConcurrencyGroup::Policy::StopLowestPriority:
                    evict = oldestLowest([&](const Voice& v) { return v.event->group == e->group; }, e->priority, false);
                    break;
            }
            if (!evict) {
                ++stats_.rejected;
                return {PlayStatus::Rejected, 0};
            }
        }
    }
    if (!evict && active_ >= size_t(config_.maxVoices)) {
        evict = oldestLowest([](const Voice&) { return true; }, e->priority, true);
        if (!evict) {
            ++stats_.rejected;
            return {PlayStatus::Rejected, 0};
        }
    }

    if (evict) {
        release(*evict);
        ++stats_.stolen;
    }
    size_t slot;
    if (!free_.empty()) {
        slot = free_.back();
        free_.pop_back();
    } else {
        slot = voices_.size();
        voices_.emplace_back();
    }
    Voice& v = voices_[slot];
    v.active = true;
    v.real = false;
    v.event = e;
    v.duration = info->duration;
    v.position = params.position;
    v.volume = params.volume;
    v.pitch = params.pitch;
    v.time = 0;
    v.order = nextOrder_++;
    ++active_;
    std::map<std::string, float> gains;  // provisional parameters until the next update()
    v.params = compute(v, gains);
    return {PlayStatus::Started, idOf(slot)};
}

bool AudioSystem::stop(VoiceId id) {
    if (!resolve(id)) return false;
    release(size_t((id & 0xFFFFFFFFull) - 1));
    return true;
}

void AudioSystem::stopAll() {
    for (size_t i = 0; i < voices_.size(); ++i)
        if (voices_[i].active) release(i);
}

bool AudioSystem::setVoicePosition(VoiceId id, const Vec3& position) {
    Voice* v = resolve(id);
    if (!v || !finite(position) || !inExtent(position, config_.worldExtent)) return false;
    v->position = position;
    return true;
}

bool AudioSystem::playing(VoiceId id) const { return resolve(id) != nullptr; }

std::optional<VoiceInfo> AudioSystem::voice(VoiceId id) const {
    const Voice* v = resolve(id);
    if (!v) return std::nullopt;
    return VoiceInfo{v->event->name, v->real, v->time, v->params};
}

void AudioSystem::setBusVolume(const std::string& bus, float volume) {
    auto it = busState_.find(bus);
    if (it == busState_.end()) fail("unknown bus '" + bus + "'");
    if (!inRange(volume, 0, 4)) fail("bus volume must be in [0, 4]");
    it->second.volume = volume;
}

void AudioSystem::setBusMuted(const std::string& bus, bool muted) {
    auto it = busState_.find(bus);
    if (it == busState_.end()) fail("unknown bus '" + bus + "'");
    it->second.muted = muted;
}

void AudioSystem::setListener(const Listener& l) {
    if (!finite(l.position) || !inExtent(l.position, config_.worldExtent)) fail("listener position is invalid");
    if (!finite(l.forward) || !finite(l.up) || !(length(l.forward) > 1e-6f) || !(length(l.up) > 1e-6f))
        fail("listener orientation must be finite and non-zero");
    if (!(length(cross(normalize(l.forward), normalize(l.up))) > 1e-4f)) fail("listener forward and up are parallel");
    if (!inRange(l.gain, 0, 4)) fail("listener gain must be in [0, 4]");
    listener_ = l;
}

void AudioSystem::update(double dt) {
    if (!inRange(dt, 0, 1)) fail("audio update dt must be finite and in [0, 1] seconds");

    // Effective bus gains (parents precede children in the bank).
    std::map<std::string, float> gains;
    for (const Bus& b : bank_.buses()) {
        const Bus& s = busState_.at(b.name);
        const float parent = b.parent.empty() ? 1.0f : gains.at(b.parent);
        gains[b.name] = s.muted ? 0.0f : parent * s.volume;
    }

    // Advance time; finished one-shots end (in slot order, so backend stop calls are deterministic).
    for (size_t i = 0; i < voices_.size(); ++i) {
        Voice& v = voices_[i];
        if (!v.active) continue;
        v.time += dt * double(v.event->pitch) * double(v.pitch);
        if (v.event->looping) {
            v.time = std::fmod(v.time, v.duration);
        } else if (v.time >= v.duration) {
            release(i);
        }
    }

    std::vector<size_t> ranked;
    ranked.reserve(active_);
    for (size_t i = 0; i < voices_.size(); ++i) {
        Voice& v = voices_[i];
        if (!v.active) continue;
        v.params = compute(v, gains);
        ranked.push_back(i);
    }
    std::sort(ranked.begin(), ranked.end(), [&](size_t a, size_t b) {
        const Voice& x = voices_[a];
        const Voice& y = voices_[b];
        if (x.event->priority != y.event->priority) return x.event->priority > y.event->priority;
        if (x.params.gain != y.params.gain) return x.params.gain > y.params.gain;
        return x.order < y.order;
    });
    std::vector<char> selected(voices_.size(), 0);
    int budget = config_.maxRealVoices;
    for (size_t i : ranked) {
        if (budget == 0) break;
        if (!(voices_[i].params.gain > 0)) continue;  // inaudible voices never take a real slot
        selected[i] = 1;
        --budget;
    }

    // 1. Demote (stop) in slot order, 2. promote (start) in rank order, 3. update the rest in slot order.
    for (size_t i = 0; i < voices_.size(); ++i) {
        Voice& v = voices_[i];
        if (v.active && v.real && !selected[i]) {
            v.real = false;
            backend_->stopVoice(idOf(i));
        }
    }
    std::vector<char> started(voices_.size(), 0);
    for (size_t i : ranked) {
        Voice& v = voices_[i];
        if (!selected[i] || v.real) continue;
        if (backend_->startVoice(idOf(i), v.event->asset, v.params)) {
            v.real = true;
            started[i] = 1;
        } else {
            ++stats_.backendFailures;
        }
    }
    stats_.realVoices = stats_.virtualVoices = 0;
    for (size_t i = 0; i < voices_.size(); ++i) {
        const Voice& v = voices_[i];
        if (!v.active) continue;
        if (v.real) {
            ++stats_.realVoices;
            if (!started[i]) backend_->updateVoice(idOf(i), v.params);
        } else {
            ++stats_.virtualVoices;
        }
    }
}

}  // namespace titan::audio
