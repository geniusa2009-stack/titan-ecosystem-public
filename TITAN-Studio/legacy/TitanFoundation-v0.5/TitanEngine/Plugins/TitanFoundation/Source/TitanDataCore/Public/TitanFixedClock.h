#pragma once
// Fixed update schedule with bounded catch-up and deterministic step counts (within one platform).
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Titan {
struct ClockResult { int Steps = 0; double Interpolation = 0; double DroppedSeconds = 0; };
class FixedClock {
public:
    FixedClock(double stepSeconds = 1.0 / 60.0, int maxSteps = 5, double maxFrameSeconds = 0.25)
        : Step_(stepSeconds), MaxSteps_(maxSteps), MaxFrame_(maxFrameSeconds) {
        if (!std::isfinite(stepSeconds) || stepSeconds <= 0 || maxSteps < 1 ||
            !std::isfinite(maxFrameSeconds) || maxFrameSeconds <= 0)
            throw std::invalid_argument("invalid clock configuration");
    }
    template<class Fn> ClockResult Advance(double elapsed, Fn tick) {
        if (!std::isfinite(elapsed) || elapsed < 0) throw std::invalid_argument("invalid frame time");
        ClockResult result;
        const double frame = std::min(elapsed, MaxFrame_);
        result.DroppedSeconds = elapsed - frame;
        Remainder_ += frame;
        // Keep only bounded backlog, to avoid spiral of death after an expensive frame.
        const double maxBacklog = Step_ * MaxSteps_;
        if (Remainder_ > maxBacklog) {
            result.DroppedSeconds += Remainder_ - maxBacklog;
            Remainder_ = maxBacklog;
        }
        const double tolerance = Step_ * 1e-8;
        while (result.Steps < MaxSteps_ && Remainder_ + tolerance >= Step_) {
            tick(Step_);
            Remainder_ -= Step_;
            ++result.Steps;
        }
        if (Remainder_ < 0) Remainder_ = 0;
        result.Interpolation = std::min(1.0, Remainder_ / Step_);
        return result;
    }
    void Reset() { Remainder_ = 0; }
private:
    double Step_;
    int MaxSteps_;
    double MaxFrame_;
    double Remainder_ = 0;
};
} // namespace Titan
