#pragma once
// Original TITAN Engine code. No Unreal or third-party header dependencies.
// A bounded sliding window records measured streaming operation latency.
#include <array>
#include <cstddef>
#include <cstdint>

namespace Titan {

struct StreamingLatency {
    std::uint64_t Started = 0;
    std::uint64_t Succeeded = 0;
    std::uint64_t Failed = 0;
    std::uint64_t TimedOut = 0;
    std::uint64_t InFlight = 0;
    std::size_t Samples = 0;
    double MeanMs = 0.0;
    double P95Ms = 0.0;
    double MaxMs = 0.0;
};

struct StreamingSummary {
    StreamingLatency Loads;
    StreamingLatency Unloads;
};

class StreamingTelemetry {
public:
    static constexpr std::size_t WindowSize = 256;

    void Begin(bool load) noexcept;
    // Returns false if the finish call is unmatched. An invalid duration is
    // recorded as a failed operation rather than corrupting the sample window.
    bool Finish(bool load, bool success, bool timedOut, double durationSeconds) noexcept;
    StreamingSummary Snapshot() const;
    void Reset() noexcept;

private:
    struct Bucket {
        std::uint64_t started = 0;
        std::uint64_t succeeded = 0;
        std::uint64_t failed = 0;
        std::uint64_t timedOut = 0;
        std::uint64_t inFlight = 0;
        std::array<double, WindowSize> samples{};
        std::size_t used = 0;
        std::size_t next = 0;
    };
    Bucket loads_{};
    Bucket unloads_{};
    static StreamingLatency Summarize(const Bucket& bucket);
};

} // namespace Titan
