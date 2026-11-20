// TITAN Engine — audio core: sound definitions, buses, concurrency groups, spatialization and voice management.
// Original TITAN code.
//
// Scope. This layer decides *what* should be heard and *how loud / where*: event definitions, bus hierarchy,
// distance attenuation, panning, priorities and the voice budget. Decoding, DSP, mixing and output are behind
// AudioBackend; the Unreal integration implements it on the Unreal Audio Engine (whose mixer, submixes and
// decoders are reused, not duplicated). NullAudioBackend renders nothing. No sound is played by this code alone.
//
// Model:
//  * AudioBank (immutable once handed to an AudioSystem): buses (tree rooted at "master"), concurrency groups and
//    sound events. Validated and serialized as "titan.audio" v1.
//  * AudioSystem owns every voice. play() validates, resolves the asset and admits the voice (concurrency group,
//    then total capacity); it starts *virtual*. update(dt) advances time, ends finished one-shots, computes render
//    parameters and gives the backend the `maxRealVoices` most important audible voices; the rest stay virtual
//    (tracked and timed, not rendered) and resume at the right playback time when they become important again.
//  * Ranking is deterministic: priority (high first), then gain (loud first), then start order (old first).
//  * Handles are generational: a stale handle is never confused with a newer voice (a slot's generation wraps
//    after 2^32 reuses, the only theoretical aliasing case).
//  * Missing assets are predictable: play() returns PlayStatus::MissingAsset, creates no voice and calls nothing
//    on the backend. A backend that fails to start a voice leaves it virtual (counted in stats), never lost.
//
// Ownership and threading: AudioSystem owns its backend and keeps a shared reference to the asset provider. It is
// not copyable or movable (handles refer to it) and stops every backend voice in its destructor. All calls come
// from one (game) thread; a backend that renders on an audio thread must do its own hand-off.
// Not exposed over the protocol yet: no capability is advertised until it is wired and verified.
#pragma once

#include "titan/json.h"
#include "titan/math.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace titan::audio {

// ---- assets ------------------------------------------------------------------------------------------------------

// Names an audio asset and the minimum asset revision the definition was authored against.
struct AssetRef {
    std::string path;      // 1-512 characters, no NUL / backslash, no ".." segment
    uint32_t revision = 1;  // >= 1
    bool operator==(const AssetRef& o) const { return path == o.path && revision == o.revision; }
};

struct AssetInfo {
    double duration = 0;  // seconds, finite and > 0
    int channels = 1;     // 1..8
    int sampleRate = 48000;  // 8000..192000
    uint32_t revision = 1;
};

class AssetProvider {
public:
    virtual ~AssetProvider() = default;
    // The asset, or nullopt when it is missing or older than ref.revision.
    virtual std::optional<AssetInfo> find(const AssetRef& ref) const = 0;
};

// In-memory provider for tools and tests.
class MapAssetProvider : public AssetProvider {
public:
    void put(const std::string& path, const AssetInfo& info);  // std::invalid_argument for invalid info
    void erase(const std::string& path) { assets_.erase(path); }
    std::optional<AssetInfo> find(const AssetRef& ref) const override;

private:
    std::map<std::string, AssetInfo> assets_;
};

// ---- definitions -------------------------------------------------------------------------------------------------

struct Attenuation {
    enum class Model { None, Linear, Inverse, Custom };
    Model model = Model::Inverse;
    float minDistance = 1;    // full volume inside (> 0 for Inverse)
    float maxDistance = 100;  // silent beyond (all models but None)
    float rolloff = 1;        // Inverse only, (0, 100]
    std::vector<std::pair<float, float>> points;  // Custom: 2..64 (distance, gain), distances strictly increasing

    float gain(float distance) const;  // in [0, 1]; distance must be finite and >= 0
};

struct Bus {
    std::string name;
    std::string parent = "master";  // empty only for "master"
    float volume = 1;               // [0, 4]
    bool muted = false;
};

struct ConcurrencyGroup {
    enum class Policy { RejectNew, StopOldest, StopLowestPriority };
    std::string name;
    int maxVoices = 8;  // [1, 4096]
    Policy policy = Policy::StopOldest;
};

struct SoundEvent {
    std::string name;
    AssetRef asset;
    std::string bus = "master";
    std::string group;   // optional concurrency group
    float volume = 1;    // [0, 4]
    float pitch = 1;     // [0.125, 8]
    bool looping = false;
    bool spatial = true;  // 3D (attenuated and panned) or 2D
    float pan = 0;        // 2D only, [-1, 1]
    float spread = 0;     // 3D only, [0, 1]: 1 removes directional panning
    int priority = 128;   // [0, 255], higher wins
    Attenuation attenuation;
};

class AudioBank {
public:
    AudioBank();  // contains the "master" bus

    // Each add validates against what is already present (parents and buses must exist first) and throws
    // std::invalid_argument without changing the bank.
    void addBus(const Bus& bus);
    void addGroup(const ConcurrencyGroup& group);
    void addEvent(const SoundEvent& event);

    const Bus* bus(const std::string& name) const;
    const ConcurrencyGroup* group(const std::string& name) const;
    const SoundEvent* event(const std::string& name) const;
    const std::vector<Bus>& buses() const { return buses_; }  // insertion order, parents first

    Json toJson() const;
    static AudioBank fromJson(const Json& j);

private:
    std::vector<Bus> buses_;
    std::vector<ConcurrencyGroup> groups_;
    std::vector<SoundEvent> events_;
    std::map<std::string, size_t> busIndex_, groupIndex_, eventIndex_;
};

// ---- runtime -----------------------------------------------------------------------------------------------------

struct Listener {
    Vec3 position;
    Vec3 forward{0, 0, -1};
    Vec3 up{0, 1, 0};
    float gain = 1;  // [0, 4]
};

struct RenderParams {
    float gain = 0;       // final linear gain after event, voice, bus, listener and attenuation
    float pitch = 1;      // playback rate
    float pan = 0;        // -1 left .. +1 right
    float distance = 0;   // metres (0 for 2D)
    float azimuth = 0;    // degrees, 0 ahead, +90 right, +/-180 behind (0 for 2D)
    float elevation = 0;  // degrees, +90 above (0 for 2D)
    bool looping = false;
    double playbackTime = 0;  // seconds into the asset where rendering should be
};

using VoiceId = uint64_t;  // handle: (generation << 32) | slot + 1; 0 is never valid

class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    // Returns false when the voice cannot be rendered now (it stays virtual and is retried later).
    virtual bool startVoice(VoiceId id, const AssetRef& asset, const RenderParams& params) = 0;
    virtual void updateVoice(VoiceId id, const RenderParams& params) = 0;
    virtual void stopVoice(VoiceId id) = 0;
};

// Accepts every voice and renders nothing.
class NullAudioBackend : public AudioBackend {
public:
    bool startVoice(VoiceId, const AssetRef&, const RenderParams&) override { return true; }
    void updateVoice(VoiceId, const RenderParams&) override {}
    void stopVoice(VoiceId) override {}
};

struct AudioConfig {
    int maxVoices = 256;     // real + virtual, [1, 65536]
    int maxRealVoices = 32;  // rendered by the backend, [0, maxVoices]
    float worldExtent = 1e6f;  // |coordinate| limit for emitters and the listener
};

enum class PlayStatus { Started, UnknownEvent, MissingAsset, Rejected, InvalidParams };

struct PlayParams {
    Vec3 position;       // 3D events
    float volume = 1;    // [0, 4], multiplies the event volume
    float pitch = 1;     // [0.125, 8], multiplies the event pitch
};

struct PlayResult {
    PlayStatus status = PlayStatus::Rejected;
    VoiceId voice = 0;  // non-zero only when Started
};

struct VoiceInfo {
    std::string event;
    bool real = false;
    double time = 0;
    RenderParams params;  // as of the latest update()
};

struct AudioStats {
    int realVoices = 0;
    int virtualVoices = 0;
    uint64_t missingAssets = 0;
    uint64_t rejected = 0;
    uint64_t stolen = 0;           // voices stopped to admit higher-priority or newer ones
    uint64_t backendFailures = 0;  // startVoice() returned false
};

class AudioSystem {
public:
    // std::invalid_argument for invalid config or null backend / provider.
    AudioSystem(AudioBank bank, std::unique_ptr<AudioBackend> backend, std::shared_ptr<const AssetProvider> assets,
                AudioConfig config = {});
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    const AudioBank& bank() const { return bank_; }
    AudioBackend& backend() { return *backend_; }

    // Never throws for bad input: it reports a status instead (play is called from gameplay code).
    PlayResult play(const std::string& event, const PlayParams& params = {});
    bool stop(VoiceId voice);  // false for unknown / stale handles
    void stopAll();
    bool setVoicePosition(VoiceId voice, const Vec3& position);  // false for stale handles or invalid positions
    bool playing(VoiceId voice) const;
    std::optional<VoiceInfo> voice(VoiceId voice) const;

    // std::invalid_argument for unknown buses or invalid values.
    void setBusVolume(const std::string& bus, float volume);
    void setBusMuted(const std::string& bus, bool muted);
    void setListener(const Listener& listener);  // std::invalid_argument for non-finite / degenerate input
    const Listener& listener() const { return listener_; }

    // dt finite in [0, 1] seconds (std::invalid_argument otherwise, nothing changes).
    void update(double dt);
    const AudioStats& stats() const { return stats_; }
    size_t voiceCount() const { return active_; }

private:
    struct Voice {
        uint32_t generation = 1;
        bool active = false;
        bool real = false;
        const SoundEvent* event = nullptr;
        double duration = 0;
        Vec3 position;
        float volume = 1, pitch = 1;
        double time = 0;
        uint64_t order = 0;
        RenderParams params;
    };
    VoiceId idOf(size_t slot) const;
    Voice* resolve(VoiceId id);
    const Voice* resolve(VoiceId id) const;
    void release(size_t slot);
    RenderParams compute(const Voice& v, const std::map<std::string, float>& busGains) const;

    AudioBank bank_;
    std::unique_ptr<AudioBackend> backend_;
    std::shared_ptr<const AssetProvider> assets_;
    AudioConfig config_;
    std::map<std::string, Bus> busState_;  // runtime volume / mute; the bank itself never changes
    Listener listener_;
    std::vector<Voice> voices_;
    std::vector<size_t> free_;
    size_t active_ = 0;
    uint64_t nextOrder_ = 1;
    AudioStats stats_;
};

}  // namespace titan::audio
