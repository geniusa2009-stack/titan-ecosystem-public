#include "TitanStreamingTelemetry.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace Titan {

void StreamingTelemetry::Begin(bool load) noexcept {
    Bucket& bucket = load ? loads_ : unloads_;
    ++bucket.started;
    ++bucket.inFlight;
}

bool StreamingTelemetry::Finish(bool load, bool success, bool timedOut,
                                double durationSeconds) noexcept {
    Bucket& bucket = load ? loads_ : unloads_;
    if (bucket.inFlight == 0) return false;
    --bucket.inFlight;
    const bool valid = std::isfinite(durationSeconds) && durationSeconds >= 0.0;
    if (timedOut) ++bucket.timedOut;
    if (success && valid && !timedOut) ++bucket.succeeded;
    else ++bucket.failed;
    if (valid) {
        bucket.samples[bucket.next] = durationSeconds * 1000.0;
        bucket.next = (bucket.next + 1) % WindowSize;
        bucket.used = bucket.used < WindowSize ? bucket.used + 1 : WindowSize;
    }
    return true;
}

StreamingLatency StreamingTelemetry::Summarize(const Bucket& b) {
    StreamingLatency s;
    s.Started = b.started;
    s.Succeeded = b.succeeded;
    s.Failed = b.failed;
    s.TimedOut = b.timedOut;
    s.InFlight = b.inFlight;
    s.Samples = b.used;
    if (b.used == 0) return s;
    std::vector<double> sample(b.samples.begin(), b.samples.begin() + b.used);
    double total = 0.0;
    for (double x : sample) total += x;
    std::sort(sample.begin(), sample.end());
    s.MeanMs = total / sample.size();
    // Nearest-rank P95, even for a one-element window.
    const std::size_t rank = static_cast<std::size_t>(std::ceil(sample.size() * 0.95));
    s.P95Ms = sample[rank - 1];
    s.MaxMs = sample.back();
    return s;
}

StreamingSummary StreamingTelemetry::Snapshot() const {
    StreamingSummary result;
    result.Loads = Summarize(loads_);
    result.Unloads = Summarize(unloads_);
    return result;
}

void StreamingTelemetry::Reset() noexcept {
    loads_ = Bucket{};
    unloads_ = Bucket{};
}

} // namespace Titan
