// TITAN Engine — animation timeline: transform keyframes, interpolation and a deterministic playback clock.
// Original TITAN code.
//
// A Timeline holds tracks; each track animates one transform channel (position, rotation or scale) of one scene
// object with keys at strictly increasing times inside [0, duration]. Sampling clamps to the first/last key.
// The interpolation mode of a key applies to the segment that starts at it:
//   step   - hold the key's value until the next key
//   linear - straight blend
//   smooth - smoothstep ease-in/out (zero tangents at both keys)
// Playback is a pure clock (no wall time): advance(dt) is deterministic for the same sequence of dt values.
// Not exposed over the protocol yet: no capability is advertised until it is wired and verified.
#pragma once

#include "titan/scene.h"

#include <vector>

namespace titan {

enum class Interp { Step, Linear, Smooth };
enum class Channel { Position, Rotation, Scale };

struct Keyframe {
    double time = 0;
    Vec3 value;
    Interp interp = Interp::Linear;
};

struct Track {
    ObjectId target = kNoObject;
    Channel channel = Channel::Position;
    std::vector<Keyframe> keys;

    Vec3 sample(double t) const;  // requires at least one key; std::invalid_argument for non-finite t
};

struct Timeline {
    double duration = 1.0;
    std::vector<Track> tracks;

    // std::invalid_argument: non-positive/non-finite duration, a track without keys or target, keys out of range,
    // not strictly increasing or non-finite, or two tracks animating the same (target, channel).
    void validate() const;
    // Writes every track's sample at `t` into `scene`, all or nothing: throws std::invalid_argument before changing
    // anything when `t` is not finite, the timeline is invalid or a target is missing. Bumps the revision once.
    void apply(double t, Scene& scene) const;

    // {"format":"titan.timeline","version":1,"duration":..,"tracks":[{"target","channel","keys":[{"t","v","interp"}]}]}
    Json toJson() const;
    static Timeline fromJson(const Json& j);  // validates
};

class Playback {
public:
    explicit Playback(double duration, bool loop = false);

    double time() const { return time_; }
    bool playing() const { return playing_; }
    bool loop() const { return loop_; }
    double speed() const { return speed_; }

    void play() { playing_ = true; }
    void pause() { playing_ = false; }
    void stop() { playing_ = false; time_ = 0; }
    void seek(double t);           // clamped to [0, duration]
    void setSpeed(double speed);   // finite, may be negative (reverse); throws std::invalid_argument otherwise
    // Advances by dt * speed while playing. Looping wraps; otherwise the clock clamps at an end and pauses.
    // std::invalid_argument (state unchanged) for a negative or non-finite dt or a step that overflows.
    void advance(double dt);

private:
    double duration_;
    double time_ = 0;
    double speed_ = 1;
    bool loop_;
    bool playing_ = false;
};

}  // namespace titan
