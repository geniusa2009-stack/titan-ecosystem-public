// TITAN audio core tests. Original TITAN code.
#include "titan/audio.h"
#include "titan_test.h"

#include <limits>

using namespace titan;
using namespace titan::audio;

namespace {
struct Log {
    std::vector<std::string> calls;
    std::map<VoiceId, RenderParams> last;
    int live = 0;
    bool refuse = false;
};

class RecordingBackend : public AudioBackend {
public:
    explicit RecordingBackend(std::shared_ptr<Log> log) : log_(std::move(log)) {}
    bool startVoice(VoiceId id, const AssetRef&, const RenderParams& p) override {
        if (log_->refuse) return false;
        log_->calls.push_back("start:" + std::to_string(id));
        log_->last[id] = p;
        ++log_->live;
        return true;
    }
    void updateVoice(VoiceId id, const RenderParams& p) override { log_->last[id] = p; }
    void stopVoice(VoiceId id) override {
        log_->calls.push_back("stop:" + std::to_string(id));
        --log_->live;
    }

private:
    std::shared_ptr<Log> log_;
};

std::shared_ptr<MapAssetProvider> assets() {
    auto a = std::make_shared<MapAssetProvider>();
    AssetInfo one;
    one.duration = 1.0;
    a->put("sfx/boom.wav", one);
    AssetInfo longer;
    longer.duration = 10.0;
    a->put("music/theme.ogg", longer);
    return a;
}

SoundEvent ev(const char* name, const char* path = "sfx/boom.wav", int priority = 128) {
    SoundEvent e;
    e.name = name;
    e.asset.path = path;
    e.priority = priority;
    return e;
}

struct Rig {
    std::shared_ptr<Log> log = std::make_shared<Log>();
    std::unique_ptr<AudioSystem> sys;
    Rig(AudioBank bank, AudioConfig cfg = {}, std::shared_ptr<const AssetProvider> a = assets()) {
        sys = std::make_unique<AudioSystem>(std::move(bank), std::make_unique<RecordingBackend>(log), std::move(a), cfg);
    }
};
}  // namespace

TEST("audio: attenuation curves") {
    Attenuation none;
    none.model = Attenuation::Model::None;
    CHECK(none.gain(1e5f) == 1);
    Attenuation lin;
    lin.model = Attenuation::Model::Linear;
    lin.minDistance = 1;
    lin.maxDistance = 11;
    CHECK(lin.gain(0) == 1 && lin.gain(1) == 1 && lin.gain(11) == 0 && lin.gain(20) == 0);
    CHECK_NEAR(lin.gain(6), 0.5, 1e-6);
    Attenuation inv;  // defaults: inverse, min 1, max 100, rolloff 1
    CHECK(inv.gain(0.5f) == 1 && inv.gain(100) == 0);
    CHECK_NEAR(inv.gain(2), 0.5, 1e-6);
    CHECK_NEAR(inv.gain(4), 0.25, 1e-6);
    Attenuation custom;
    custom.model = Attenuation::Model::Custom;
    custom.points = {{0, 1}, {10, 0.5f}, {20, 0}};
    CHECK_NEAR(custom.gain(5), 0.75, 1e-6);
    CHECK_NEAR(custom.gain(15), 0.25, 1e-6);
    CHECK(custom.gain(30) == 0);
}

TEST("audio: bank validation refuses bad definitions without changing the bank") {
    AudioBank bank;
    Bus sfx;
    sfx.name = "sfx";
    bank.addBus(sfx);
    CHECK_THROWS(bank.addBus(sfx));  // duplicate
    Bus orphan;
    orphan.name = "orphan";
    orphan.parent = "nowhere";
    CHECK_THROWS(bank.addBus(orphan));
    Bus loud;
    loud.name = "loud";
    loud.volume = 5;
    CHECK_THROWS(bank.addBus(loud));
    std::string parent = "sfx";
    for (int i = 0; i < 15; ++i) {
        Bus b;
        b.name = "n" + std::to_string(i);
        b.parent = parent;
        bank.addBus(b);
        parent = b.name;
    }
    Bus tooDeep;
    tooDeep.name = "deep";
    tooDeep.parent = parent;
    CHECK_THROWS(bank.addBus(tooDeep));
    ConcurrencyGroup g;
    g.name = "steps";
    g.maxVoices = 0;
    CHECK_THROWS(bank.addGroup(g));

    std::vector<SoundEvent> bad;
    auto with = [&](auto edit) {
        SoundEvent e = ev("bad");
        edit(e);
        bad.push_back(e);
    };
    with([](SoundEvent& e) { e.asset.path = ""; });
    with([](SoundEvent& e) { e.asset.path = "../secret.wav"; });
    with([](SoundEvent& e) { e.asset.path = "sfx/../../x.wav"; });
    with([](SoundEvent& e) { e.asset.path = "sfx\\boom.wav"; });
    with([](SoundEvent& e) { e.asset.revision = 0; });
    with([](SoundEvent& e) { e.bus = "missing"; });
    with([](SoundEvent& e) { e.group = "missing"; });
    with([](SoundEvent& e) { e.volume = std::numeric_limits<float>::quiet_NaN(); });
    with([](SoundEvent& e) { e.pitch = 0; });
    with([](SoundEvent& e) { e.pan = 2; });
    with([](SoundEvent& e) { e.priority = 256; });
    with([](SoundEvent& e) { e.attenuation.minDistance = 0; });  // inverse needs min > 0
    with([](SoundEvent& e) { e.attenuation.maxDistance = 0.5f; });
    with([](SoundEvent& e) {
        e.attenuation.model = Attenuation::Model::Custom;
        e.attenuation.points = {{5, 1}, {5, 0}};
    });
    with([](SoundEvent& e) {
        e.attenuation.model = Attenuation::Model::Custom;
        e.attenuation.points = {{0, 1}, {5, 1.5f}};
    });
    with([](SoundEvent& e) { e.name = "has space"; });
    for (const SoundEvent& e : bad) CHECK_THROWS(bank.addEvent(e));
    CHECK(!bank.event("bad"));
    bank.addEvent(ev("ok"));
    CHECK_THROWS(bank.addEvent(ev("ok")));
    CHECK(bank.event("ok") != nullptr);
}

TEST("audio: bank json round trip and refusal of malformed documents") {
    AudioBank bank;
    Bus music;
    music.name = "music";
    music.volume = 0.5f;
    music.muted = true;
    bank.addBus(music);
    ConcurrencyGroup g;
    g.name = "footsteps";
    g.maxVoices = 3;
    g.policy = ConcurrencyGroup::Policy::StopLowestPriority;
    bank.addGroup(g);
    SoundEvent theme = ev("theme", "music/theme.ogg", 250);
    theme.bus = "music";
    theme.looping = true;
    theme.spatial = false;
    theme.pan = -0.25f;
    theme.asset.revision = 3;
    bank.addEvent(theme);
    SoundEvent step = ev("step");
    step.group = "footsteps";
    step.attenuation.model = Attenuation::Model::Custom;
    step.attenuation.points = {{0, 1}, {4, 0.5f}, {30, 0}};
    bank.addEvent(step);
    const Json j = bank.toJson();
    const AudioBank back = AudioBank::fromJson(Json::parse(j.dump()));
    CHECK(back.toJson() == j);
    CHECK(back.event("theme")->asset.revision == 3 && back.bus("music")->muted);
    Json v2 = j;
    v2["version"] = 2;
    CHECK_THROWS(AudioBank::fromJson(v2));
    Json badPolicy = j;
    badPolicy["groups"].asArray()[0]["policy"] = "random";
    CHECK_THROWS(AudioBank::fromJson(badPolicy));
    Json badModel = j;
    badModel["events"].asArray()[1]["attenuation"]["model"] = "cubic";
    CHECK_THROWS(AudioBank::fromJson(badModel));
    Json eventBeforeBus = j;
    eventBeforeBus["buses"] = Json::array();
    CHECK_THROWS(AudioBank::fromJson(eventBeforeBus));  // "music" bus is missing
}

TEST("audio: 3D positioning, panning, attenuation, buses and 2D events") {
    AudioBank bank;
    Bus sfx;
    sfx.name = "sfx";
    sfx.volume = 0.5f;
    bank.addBus(sfx);
    SoundEvent e = ev("hit");
    e.bus = "sfx";
    bank.addEvent(e);
    SoundEvent wide = ev("wide");
    wide.spread = 1;
    bank.addEvent(wide);
    SoundEvent ui = ev("ui");
    ui.spatial = false;
    ui.pan = 0.3f;
    bank.addEvent(ui);
    Rig rig(bank);
    AudioSystem& s = *rig.sys;
    PlayParams right;
    right.position = {2, 0, 0};
    const VoiceId r = s.play("hit", right).voice;
    PlayParams ahead;
    ahead.position = {0, 0, -2};
    const VoiceId a = s.play("hit", ahead).voice;
    PlayParams above;
    above.position = {0, 2, 0};
    const VoiceId u = s.play("hit", above).voice;
    PlayParams behind;
    behind.position = {0, 0, 2};
    const VoiceId b = s.play("hit", behind).voice;
    const VoiceId w = s.play("wide", right).voice;
    PlayParams far;
    far.position = {1000, 0, 0};
    const VoiceId f = s.play("ui", far).voice;
    s.update(0);
    const RenderParams pr = s.voice(r)->params;
    CHECK_NEAR(pr.distance, 2, 1e-6);
    CHECK_NEAR(pr.gain, 0.25, 1e-6);  // 0.5 (inverse, d=2) * 0.5 (bus)
    CHECK_NEAR(pr.pan, 1, 1e-6);
    CHECK_NEAR(pr.azimuth, 90, 1e-4);
    CHECK_NEAR(s.voice(a)->params.pan, 0, 1e-6);
    CHECK_NEAR(s.voice(a)->params.azimuth, 0, 1e-4);
    CHECK_NEAR(s.voice(u)->params.elevation, 90, 1e-3);
    CHECK_NEAR(std::fabs(s.voice(b)->params.azimuth), 180, 1e-4);
    CHECK_NEAR(s.voice(w)->params.pan, 0, 1e-6);
    const RenderParams pu = s.voice(f)->params;
    CHECK(pu.distance == 0 && pu.gain == 1 && pu.pan == 0.3f);  // 2D: unattenuated, fixed pan
    Listener l;
    l.forward = {1, 0, 0};
    s.setListener(l);
    s.update(0);
    CHECK_NEAR(s.voice(b)->params.pan, 1, 1e-6);  // +Z is to the right when facing +X
    s.setBusVolume("master", 0.5f);
    s.update(0);
    CHECK_NEAR(s.voice(r)->params.gain, 0.125, 1e-6);
    s.setBusMuted("master", true);
    s.update(0);
    CHECK(s.voice(r)->params.gain == 0 && s.voice(f)->params.gain == 0);
    CHECK(s.stats().realVoices == 0);  // muted voices never hold a real slot
    CHECK_THROWS(s.setBusVolume("nope", 1));
    CHECK_THROWS(s.setBusVolume("master", 5));
    Listener parallel;
    parallel.forward = {0, 1, 0};
    CHECK_THROWS(s.setListener(parallel));
}

TEST("audio: voice budget follows priority, gain and age, and virtual voices resume in time") {
    AudioBank bank;
    bank.addEvent(ev("low", "music/theme.ogg", 100));
    bank.addEvent(ev("high", "music/theme.ogg", 200));
    AudioConfig cfg;
    cfg.maxRealVoices = 2;
    Rig rig(bank, cfg);
    AudioSystem& s = *rig.sys;
    const VoiceId a = s.play("low").voice;
    const VoiceId b = s.play("low").voice;
    const VoiceId c = s.play("high").voice;
    CHECK(rig.log->calls.empty());  // voices start virtual; update() decides
    s.update(0.25);
    const std::vector<std::string> first{"start:" + std::to_string(c), "start:" + std::to_string(a)};
    CHECK(rig.log->calls == first);
    CHECK(s.voice(a)->real && !s.voice(b)->real && s.voice(c)->real);
    CHECK(s.stats().realVoices == 2 && s.stats().virtualVoices == 1);
    s.update(0.25);
    CHECK(s.stop(c));
    s.update(0.5);
    CHECK(s.voice(b)->real);
    CHECK_NEAR(rig.log->last.at(b).playbackTime, 1.0, 1e-9);  // resumed where it would have been
    PlayParams far;
    far.position = {500, 0, 0};  // beyond the inverse curve's 100 m: inaudible
    const VoiceId silent = s.play("high", far).voice;
    s.update(0);
    CHECK(!s.voice(silent)->real);
    CHECK(rig.log->live == 2);
}

TEST("audio: concurrency groups and the total voice limit") {
    AudioBank bank;
    ConcurrencyGroup oldest;
    oldest.name = "oldest";
    oldest.maxVoices = 2;
    bank.addGroup(oldest);
    ConcurrencyGroup reject = oldest;
    reject.name = "reject";
    reject.policy = ConcurrencyGroup::Policy::RejectNew;
    bank.addGroup(reject);
    ConcurrencyGroup prio = oldest;
    prio.name = "prio";
    prio.policy = ConcurrencyGroup::Policy::StopLowestPriority;
    bank.addGroup(prio);
    SoundEvent o = ev("o");
    o.group = "oldest";
    bank.addEvent(o);
    SoundEvent r = ev("r");
    r.group = "reject";
    bank.addEvent(r);
    SoundEvent pl = ev("pl", "sfx/boom.wav", 50);
    pl.group = "prio";
    bank.addEvent(pl);
    SoundEvent ph = ev("ph", "sfx/boom.wav", 150);
    ph.group = "prio";
    bank.addEvent(ph);
    Rig rig(bank);
    AudioSystem& s = *rig.sys;
    const VoiceId o1 = s.play("o").voice;
    const VoiceId o2 = s.play("o").voice;
    const PlayResult o3 = s.play("o");
    CHECK(o3.status == PlayStatus::Started && !s.playing(o1) && s.playing(o2) && s.playing(o3.voice));
    s.play("r");
    s.play("r");
    CHECK(s.play("r").status == PlayStatus::Rejected);
    const VoiceId l1 = s.play("pl").voice;
    const VoiceId l2 = s.play("pl").voice;
    const VoiceId h1 = s.play("ph").voice;
    CHECK(!s.playing(l1) && s.playing(l2) && s.playing(h1));
    const VoiceId l3 = s.play("pl").voice;  // equal priority may replace the oldest equal one
    CHECK(!s.playing(l2) && s.playing(l3));
    s.play("ph");  // now {h1, h2}
    CHECK(s.play("pl").status == PlayStatus::Rejected);
    CHECK(s.stats().stolen == 4 && s.stats().rejected == 2);

    AudioBank small;
    small.addEvent(ev("low", "sfx/boom.wav", 10));
    small.addEvent(ev("high", "sfx/boom.wav", 20));
    AudioConfig cfg;
    cfg.maxVoices = 2;
    cfg.maxRealVoices = 2;
    Rig cap(small, cfg);
    const VoiceId c1 = cap.sys->play("low").voice;
    cap.sys->play("low");
    CHECK(cap.sys->play("low").status == PlayStatus::Rejected);  // capacity needs strictly lower priority
    CHECK(cap.sys->play("high").status == PlayStatus::Started);
    CHECK(!cap.sys->playing(c1) && cap.sys->voiceCount() == 2);
}

TEST("audio: missing assets, unknown events and invalid parameters are predictable") {
    AudioBank bank;
    bank.addEvent(ev("gone", "sfx/missing.wav"));
    SoundEvent stale = ev("stale");
    stale.asset.revision = 2;  // provider only has revision 1
    bank.addEvent(stale);
    bank.addEvent(ev("ok"));
    Rig rig(bank);
    AudioSystem& s = *rig.sys;
    CHECK(s.play("gone").status == PlayStatus::MissingAsset);
    CHECK(s.play("stale").status == PlayStatus::MissingAsset);
    CHECK(s.play("nope").status == PlayStatus::UnknownEvent);
    PlayParams nan;
    nan.position = {std::numeric_limits<float>::quiet_NaN(), 0, 0};
    CHECK(s.play("ok", nan).status == PlayStatus::InvalidParams);
    PlayParams loud;
    loud.volume = 5;
    CHECK(s.play("ok", loud).status == PlayStatus::InvalidParams);
    PlayParams slow;
    slow.pitch = 0;
    CHECK(s.play("ok", slow).status == PlayStatus::InvalidParams);
    PlayParams outside;
    outside.position = {2e6f, 0, 0};
    CHECK(s.play("ok", outside).status == PlayStatus::InvalidParams);
    s.update(0.1);
    CHECK(s.voiceCount() == 0 && rig.log->calls.empty());
    CHECK(s.stats().missingAssets == 2);
}

TEST("audio: lifecycle, stale handles, looping time and destruction") {
    AudioBank bank;
    bank.addEvent(ev("shot"));
    SoundEvent loop = ev("loop");
    loop.looping = true;
    loop.pitch = 2;
    bank.addEvent(loop);
    auto log = std::make_shared<Log>();
    {
        AudioSystem s(bank, std::make_unique<RecordingBackend>(log), assets());
        const VoiceId shot = s.play("shot").voice;
        const VoiceId lp = s.play("loop").voice;
        s.update(0.6);
        CHECK(s.playing(shot) && s.voice(shot)->real);
        CHECK_NEAR(s.voice(lp)->time, 0.2, 1e-9);  // 1.2 s of a 1 s loop at pitch 2
        s.update(0.6);
        CHECK(!s.playing(shot));
        CHECK(log->calls.back() == "stop:" + std::to_string(shot));
        CHECK(!s.stop(shot) && !s.voice(shot) && !s.setVoicePosition(shot, {1, 0, 0}));
        const VoiceId again = s.play("shot").voice;
        CHECK(again != shot && !s.playing(shot) && s.playing(again));  // slot reused, handle not
        CHECK(!s.setVoicePosition(again, {std::numeric_limits<float>::infinity(), 0, 0}));
        CHECK(!s.stop(0) && !s.stop(0xFFFFFFFFFFFFull));
        s.update(0.01);
        CHECK(log->live == 2);
        CHECK_THROWS(s.update(std::numeric_limits<double>::quiet_NaN()));
        CHECK_THROWS(s.update(-1));
        CHECK_THROWS(s.update(2));
    }
    CHECK(log->live == 0);  // the destructor stopped every backend voice
    CHECK_THROWS(AudioSystem(bank, nullptr, assets()));
    AudioConfig bad;
    bad.maxRealVoices = 300;
    CHECK_THROWS(AudioSystem(bank, std::make_unique<NullAudioBackend>(), assets(), bad));
}

TEST("audio: a failing backend keeps voices virtual and retries") {
    AudioBank bank;
    bank.addEvent(ev("loop", "music/theme.ogg"));
    Rig rig(bank);
    AudioSystem& s = *rig.sys;
    rig.log->refuse = true;
    const VoiceId v = s.play("loop").voice;
    s.update(0.1);
    s.update(0.1);
    CHECK(s.playing(v) && !s.voice(v)->real && s.stats().backendFailures == 2);
    rig.log->refuse = false;
    s.update(0.1);
    CHECK(s.voice(v)->real);
    CHECK_NEAR(rig.log->last.at(v).playbackTime, 0.3, 1e-9);
}

TITAN_TEST_MAIN()
