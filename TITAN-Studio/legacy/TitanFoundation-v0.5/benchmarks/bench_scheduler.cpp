#include "TitanWorldScheduler.h"
#include "TitanFrameGraph.h"
#include <chrono>
#include <iostream>
int main() {
    Titan::StreamConfig c; c.MaxResidentBytes=1024ULL*1024*1024; c.MaxTiles=128;
    Titan::WorldScheduler planner(c);
    const int iterations=5000;
    std::size_t commands=0;
    const auto t0=std::chrono::steady_clock::now();
    for (int i=0;i<iterations;++i) {
        auto work=planner.Tick((i%97)*91.0,(i%83)*71.0,60.0,12.0);
        commands += work.size();
        for (auto& cmd:work) {
            if (cmd.Operation==Titan::StreamOp::Load) planner.CompleteLoad(cmd.Key,true);
            else planner.CompleteUnload(cmd.Key,true);
        }
    }
    const auto t1=std::chrono::steady_clock::now();
    auto us=std::chrono::duration_cast<std::chrono::microseconds>(t1-t0).count();
    std::cout<<"metric,value,unit\n";
    std::cout<<"stream_ticks,"<<iterations<<",ticks\n";
    std::cout<<"stream_commands,"<<commands<<",requests\n";
    std::cout<<"stream_total,"<<us<<",microseconds\n";
    std::cout<<"stream_per_tick,"<<(static_cast<double>(us)/iterations)<<",microseconds_per_tick\n";
    // CPU-only microbenchmark, NOT game FPS or actual GPU rendering performance.
}
