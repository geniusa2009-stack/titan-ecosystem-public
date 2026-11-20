// TITAN animation timeline tests. Original TITAN code.
#include "titan/timeline.h"
#include "titan_test.h"

#include <limits>

using namespace titan;

namespace {
Keyframe key(double t, Vec3 v, Interp i = Interp::Linear) {
    Keyframe k;
    k.time = t;
    k.value = v;
    k.interp = i;
    return k;
}
Track track(ObjectId target, Channel c, std::vector<Keyframe> keys) {
    Track tr;
    tr.target = target;
    tr.channel = c;
    tr.keys = std::move(keys);
    return tr;
}
}  // namespace

TEST("timeline: step, linear and smooth interpolation with clamped ends") {
    const Vec3 zero{0, 0, 0};
    const Vec3 ten{10, 0, -4};
    const Track lin = track(1, Channel::Position, {key(1, zero), key(3, ten)});
    CHECK(lin.sample(0) == zero);
    CHECK(lin.sample(5) == ten);
    CHECK_NEAR(lin.sample(2).x, 5, 1e-5);
    CHECK_NEAR(lin.sample(2).z, -2, 1e-5);
    CHECK_NEAR(lin.sample(1.5).x, 2.5, 1e-5);
    const Track step = track(1, Channel::Position, {key(1, zero, Interp::Step), key(3, ten)});
    CHECK(step.sample(2.999) == zero);
    CHECK(step.sample(3) == ten);
    const Track smooth = track(1, Channel::Position, {key(1, zero, Interp::Smooth), key(3, ten)});
    CHECK_NEAR(smooth.sample(2).x, 5, 1e-5);       // symmetric midpoint
    CHECK_NEAR(smooth.sample(1.5).x, 1.5625, 1e-5);  // u=0.25 -> 0.15625
    const Track single = track(1, Channel::Scale, {key(0.5, ten)});
    CHECK(single.sample(0) == ten && single.sample(9) == ten);
}

TEST("timeline: validation refuses malformed tracks") {
    Timeline tl;
    tl.duration = 2;
    tl.tracks.push_back(track(1, Channel::Position, {key(0, {}), key(2, {1, 1, 1})}));
    tl.validate();
    Timeline unordered = tl;
    unordered.tracks[0].keys[1].time = 0;
    CHECK_THROWS(unordered.validate());
    Timeline outside = tl;
    outside.tracks[0].keys[1].time = 2.5;
    CHECK_THROWS(outside.validate());
    Timeline dup = tl;
    dup.tracks.push_back(dup.tracks[0]);
    CHECK_THROWS(dup.validate());
    Timeline empty = tl;
    empty.tracks[0].keys.clear();
    CHECK_THROWS(empty.validate());
    Timeline noTarget = tl;
    noTarget.tracks[0].target = kNoObject;
    CHECK_THROWS(noTarget.validate());
    Timeline badDuration = tl;
    badDuration.duration = 0;
    CHECK_THROWS(badDuration.validate());
}

TEST("timeline: apply writes sampled transforms all or nothing") {
    Scene scene;
    SceneObject a;
    a.id = scene.allocateId();
    a.name = "A";
    scene.put(a);
    Timeline tl;
    tl.duration = 2;
    tl.tracks.push_back(track(a.id, Channel::Position, {key(0, {0, 0, 0}), key(2, {4, 0, 0})}));
    tl.tracks.push_back(track(a.id, Channel::Scale, {key(0, {1, 1, 1}), key(2, {3, 3, 3})}));
    const uint64_t rev = scene.revision();
    tl.apply(1, scene);
    CHECK_NEAR(scene.find(a.id)->transform.position.x, 2, 1e-5);
    CHECK_NEAR(scene.find(a.id)->transform.scale.y, 2, 1e-5);
    CHECK(scene.revision() == rev + 1);
    const Json before = scene.find(a.id)->toJson();
    Timeline missing = tl;
    missing.tracks.push_back(track(999, Channel::Rotation, {key(0, {0, 90, 0})}));
    CHECK_THROWS(missing.apply(2, scene));
    CHECK(scene.find(a.id)->toJson() == before);
    CHECK(scene.revision() == rev + 1);
}

TEST("timeline: playback clock is deterministic, loops and clamps") {
    Playback once(2.0);
    once.advance(1);
    CHECK(once.time() == 0);  // paused: no movement
    once.play();
    once.advance(0.5);
    once.advance(0.5);
    CHECK_NEAR(once.time(), 1.0, 1e-12);
    once.advance(5);
    CHECK(once.time() == 2.0 && !once.playing());
    Playback loop(2.0, true);
    loop.play();
    loop.advance(5);
    CHECK_NEAR(loop.time(), 1.0, 1e-12);
    CHECK(loop.playing());
    loop.setSpeed(-1);
    loop.advance(1.5);
    CHECK_NEAR(loop.time(), 1.5, 1e-12);
    Playback reverse(2.0);
    reverse.seek(1);
    reverse.setSpeed(-2);
    reverse.play();
    reverse.advance(1);
    CHECK(reverse.time() == 0 && !reverse.playing());
    reverse.seek(9);
    CHECK(reverse.time() == 2.0);
    CHECK_THROWS(reverse.advance(-1));
    CHECK_THROWS(Playback(0));
}

TEST("timeline: json round trip and refusal of bad documents") {
    Timeline tl;
    tl.duration = 3;
    tl.tracks.push_back(track(5, Channel::Rotation, {key(0, {0, 0, 0}, Interp::Smooth), key(3, {0, 180, 0}, Interp::Step)}));
    const Json j = tl.toJson();
    const Timeline back = Timeline::fromJson(Json::parse(j.dump()));
    CHECK(back.toJson() == j);
    CHECK(back.tracks[0].keys[0].interp == Interp::Smooth);
    Json badInterp = j;
    badInterp["tracks"].asArray()[0]["keys"].asArray()[0]["interp"] = "bounce";
    CHECK_THROWS(Timeline::fromJson(badInterp));
    Json badChannel = j;
    badChannel["tracks"].asArray()[0]["channel"] = "color";
    CHECK_THROWS(Timeline::fromJson(badChannel));
}

TEST("timeline: non-finite times are refused without touching state (review regression)") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const Track tr = track(1, Channel::Position, {key(0, {0, 0, 0}), key(1, {1, 1, 1}), key(2, {2, 2, 2})});
    CHECK_THROWS(tr.sample(nan));
    CHECK_THROWS(tr.sample(inf));
    CHECK_THROWS(tr.sample(-inf));

    Scene scene;
    SceneObject a;
    a.id = scene.allocateId();
    scene.put(a);
    Timeline tl;
    tl.duration = 2;
    tl.tracks.push_back(track(a.id, Channel::Position, {key(0, {0, 0, 0}), key(2, {4, 0, 0})}));
    tl.apply(1, scene);
    const Json before = scene.toJson();
    const uint64_t rev = scene.revision();
    CHECK_THROWS(tl.apply(nan, scene));
    CHECK_THROWS(tl.apply(inf, scene));
    CHECK(scene.toJson() == before);
    CHECK(scene.revision() == rev);

    const double big = std::numeric_limits<double>::max();
    Playback loop(2.0, true);
    loop.setSpeed(big);
    loop.play();
    loop.advance(0.5);  // 0.5 * max is finite: wraps to a finite time
    CHECK(std::isfinite(loop.time()) && loop.time() >= 0 && loop.time() < 2.0);
    const double t0 = loop.time();
    CHECK_THROWS(loop.advance(big));  // max * max overflows: refused
    CHECK(loop.time() == t0 && loop.playing());
    CHECK_THROWS(loop.advance(nan));
    CHECK_THROWS(loop.seek(nan));
    CHECK(loop.time() == t0);
}

TITAN_TEST_MAIN()
