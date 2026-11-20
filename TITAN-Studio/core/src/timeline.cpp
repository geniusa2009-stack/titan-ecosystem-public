// TITAN Engine — animation timeline. Original TITAN code.
#include "titan/timeline.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace titan {
namespace {

void fail(const std::string& msg) { throw std::invalid_argument(msg); }

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

Vec3 lerp(const Vec3& a, const Vec3& b, float u) { return a + (b - a) * u; }

const char* channelName(Channel c) {
    switch (c) {
        case Channel::Position: return "position";
        case Channel::Rotation: return "rotation";
        case Channel::Scale: return "scale";
    }
    return "position";
}
Channel channelFrom(const std::string& s) {
    if (s == "position") return Channel::Position;
    if (s == "rotation") return Channel::Rotation;
    if (s == "scale") return Channel::Scale;
    throw JsonError("unknown timeline channel '" + s + "'");
}
const char* interpName(Interp i) {
    switch (i) {
        case Interp::Step: return "step";
        case Interp::Linear: return "linear";
        case Interp::Smooth: return "smooth";
    }
    return "linear";
}
Interp interpFrom(const std::string& s) {
    if (s == "step") return Interp::Step;
    if (s == "linear") return Interp::Linear;
    if (s == "smooth") return Interp::Smooth;
    throw JsonError("unknown interpolation '" + s + "'");
}
Json vec(const Vec3& v) { return Json(Json::Array{Json(v.x), Json(v.y), Json(v.z)}); }
Vec3 vecFrom(const Json& j) {
    if (!j.isArray() || j.size() != 3) throw JsonError("keyframe value must be [x, y, z]");
    return {float(j.at(0).asNumber()), float(j.at(1).asNumber()), float(j.at(2).asNumber())};
}

}  // namespace

Vec3 Track::sample(double t) const {
    if (keys.empty()) throw std::logic_error("Track::sample on a track without keys");
    if (!std::isfinite(t)) fail("timeline sample time must be finite");
    if (t <= keys.front().time) return keys.front().value;
    if (t >= keys.back().time) return keys.back().value;
    // First key strictly after t; the segment is [it-1, it].
    auto it = std::upper_bound(keys.begin(), keys.end(), t, [](double v, const Keyframe& k) { return v < k.time; });
    const Keyframe& a = *(it - 1);
    const Keyframe& b = *it;
    double u = (t - a.time) / (b.time - a.time);
    switch (a.interp) {
        case Interp::Step: return a.value;
        case Interp::Linear: break;
        case Interp::Smooth: u = u * u * (3 - 2 * u); break;
    }
    return lerp(a.value, b.value, float(u));
}

void Timeline::validate() const {
    if (!std::isfinite(duration) || duration <= 0) fail("timeline duration must be a positive number");
    std::set<std::pair<ObjectId, int>> seen;
    for (const auto& tr : tracks) {
        if (tr.target == kNoObject) fail("timeline track needs a target object");
        const std::string label = std::string(channelName(tr.channel)) + " track of object " + std::to_string(tr.target);
        if (!seen.emplace(tr.target, int(tr.channel)).second) fail("duplicate " + label);
        if (tr.keys.empty()) fail(label + " has no keys");
        for (size_t i = 0; i < tr.keys.size(); ++i) {
            const Keyframe& k = tr.keys[i];
            if (!std::isfinite(k.time) || k.time < 0 || k.time > duration) fail(label + ": key time outside the timeline");
            if (!finite(k.value)) fail(label + ": key value must be finite");
            if (i > 0 && !(k.time > tr.keys[i - 1].time)) fail(label + ": key times must strictly increase");
        }
    }
}

void Timeline::apply(double t, Scene& scene) const {
    if (!std::isfinite(t)) fail("timeline apply time must be finite");
    validate();
    for (const auto& tr : tracks)
        if (!scene.contains(tr.target)) fail("timeline target " + std::to_string(tr.target) + " is not in the scene");
    std::map<ObjectId, SceneObject> changed;
    for (const auto& tr : tracks) {
        auto [it, inserted] = changed.try_emplace(tr.target, *scene.find(tr.target));
        (void)inserted;
        Transform& x = it->second.transform;
        const Vec3 v = tr.sample(t);
        if (tr.channel == Channel::Position) x.position = v;
        else if (tr.channel == Channel::Rotation) x.rotation = v;
        else x.scale = v;
    }
    for (const auto& [id, obj] : changed) {
        (void)id;
        scene.put(obj);
    }
    scene.bumpRevision();
}

Json Timeline::toJson() const {
    Json j = Json::object();
    j["format"] = "titan.timeline";
    j["version"] = 1;
    j["duration"] = duration;
    Json ts = Json::array();
    for (const auto& tr : tracks) {
        Json o = Json::object();
        o["target"] = tr.target;
        o["channel"] = channelName(tr.channel);
        Json ks = Json::array();
        for (const auto& k : tr.keys) {
            Json kj = Json::object();
            kj["t"] = k.time;
            kj["v"] = vec(k.value);
            kj["interp"] = interpName(k.interp);
            ks.push_back(std::move(kj));
        }
        o["keys"] = std::move(ks);
        ts.push_back(std::move(o));
    }
    j["tracks"] = std::move(ts);
    return j;
}

Timeline Timeline::fromJson(const Json& j) {
    if (!j.isObject() || j.getString("format") != "titan.timeline") throw JsonError("not a titan.timeline document");
    const Json* version = j.find("version");
    if (!version || !version->isNumber() || version->asNumber() != 1) throw JsonError("unsupported titan.timeline version");
    Timeline tl;
    tl.duration = j["duration"].asNumber();
    const Json& ts = j["tracks"];
    if (!ts.isArray()) throw JsonError("titan.timeline needs a 'tracks' array");
    for (const Json& o : ts.asArray()) {
        if (!o.isObject() || !o["keys"].isArray()) throw JsonError("timeline track must be an object with 'keys'");
        Track tr;
        tr.target = o["target"].asUInt();
        tr.channel = channelFrom(o["channel"].asString());
        for (const Json& kj : o["keys"].asArray()) {
            if (!kj.isObject()) throw JsonError("keyframe must be an object");
            Keyframe k;
            k.time = kj["t"].asNumber();
            k.value = vecFrom(kj["v"]);
            k.interp = interpFrom(kj.getString("interp", "linear"));
            tr.keys.push_back(k);
        }
        tl.tracks.push_back(std::move(tr));
    }
    tl.validate();
    return tl;
}

Playback::Playback(double duration, bool loop) : duration_(duration), loop_(loop) {
    if (!std::isfinite(duration) || duration <= 0) fail("playback duration must be a positive number");
}

void Playback::seek(double t) {
    if (!std::isfinite(t)) fail("seek time must be finite");
    time_ = std::clamp(t, 0.0, duration_);
}

void Playback::setSpeed(double speed) {
    if (!std::isfinite(speed)) fail("playback speed must be finite");
    speed_ = speed;
}

void Playback::advance(double dt) {
    if (!std::isfinite(dt) || dt < 0) fail("advance needs a finite, non-negative dt");
    if (!playing_) return;
    // dt and speed are each finite, but their product (or the sum) can overflow; refuse before changing state.
    double t = time_ + dt * speed_;
    if (!std::isfinite(t)) fail("playback step overflowed (dt * speed is too large)");
    if (loop_) {
        t = std::fmod(t, duration_);
        if (t < 0) t += duration_;
    } else if ((speed_ > 0 && t >= duration_) || (speed_ < 0 && t <= 0)) {
        t = std::clamp(t, 0.0, duration_);
        playing_ = false;
    }
    time_ = t;
}

}  // namespace titan
