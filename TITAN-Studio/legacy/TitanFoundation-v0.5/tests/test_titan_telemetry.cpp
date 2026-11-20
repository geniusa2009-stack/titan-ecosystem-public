#include "TitanStreamingTelemetry.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

using Titan::StreamingTelemetry;

int main() {
    StreamingTelemetry t;
    auto s=t.Snapshot();
    assert(s.Loads.Started == 0 && s.Loads.P95Ms == 0.0);
    assert(!t.Finish(true, true, false, 1.0));
    t.Begin(true); t.Begin(true); t.Begin(false);
    assert(t.Snapshot().Loads.InFlight == 2);
    assert(t.Finish(true,true,false,0.010));
    assert(t.Finish(true,false,true,0.250));
    assert(t.Finish(false,true,false,0.004));
    s=t.Snapshot();
    assert(s.Loads.Started == 2 && s.Loads.Succeeded == 1);
    assert(s.Loads.Failed == 1 && s.Loads.TimedOut == 1 && s.Loads.InFlight == 0);
    assert(std::fabs(s.Loads.MeanMs - 130.0) < 1e-7);
    assert(std::fabs(s.Loads.P95Ms - 250.0) < 1e-7);
    assert(s.Unloads.Succeeded == 1 && s.Unloads.P95Ms == 4.0);
    t.Begin(true);
    assert(t.Finish(true, true, false, std::numeric_limits<double>::quiet_NaN()));
    assert(t.Snapshot().Loads.Failed == 2);
    // Bounded retention and monotonic counters in extended sessions.
    t.Reset();
    for(int i=0; i<500; ++i){t.Begin(true); assert(t.Finish(true,true,false,i/1000.0));}
    s=t.Snapshot();
    assert(s.Loads.Started == 500 && s.Loads.Samples == 256);
    assert(s.Loads.P95Ms >= 480.0 && s.Loads.MaxMs == 499.0);
    t.Reset();
    s=t.Snapshot();
    assert(s.Loads.Succeeded == 0 && s.Loads.P95Ms == 0.0);
    std::cout << "TITAN streaming telemetry tests passed\n";
    return 0;
}
