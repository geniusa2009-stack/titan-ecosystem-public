#include "TitanWorldScheduler.h"
#include "TitanFrameGraph.h"
#include "TitanTaskGraph.h"
#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("CHECK failed: ")+ #x + " at line "+std::to_string(__LINE__)); } while(0)
template<typename F> void MustThrow(F fn) {
    bool caught=false;
    try { fn(); } catch (const std::exception&) { caught=true; }
    CHECK(caught);
}
static Titan::StreamConfig SmallConfig() {
    Titan::StreamConfig c;
    c.CellWorldSize=100;
    c.LoadRadius=2;
    c.UnloadRadius=3;
    c.MaxTiles=3;
    c.MaxResidentBytes=3*8*1024*1024;
    c.EstimatedTileBytes=8*1024*1024;
    c.MaxInFlightLoads=2;
    c.MaxLoadsPerTick=2;
    c.MaxUnloadsPerTick=3;
    return c;
}
void TestStreamInitialization() {
    auto c=SmallConfig();
    Titan::WorldScheduler s(c);
    auto commands=s.Tick(0,0);
    CHECK(commands.size()==2);
    CHECK(commands[0].Operation==Titan::StreamOp::Load);
    CHECK(commands[0].Key==Titan::Cell(0,0));
    CHECK(s.Stats().Loading==2);
    CHECK(s.Stats().ReservedBytes==16*1024*1024);
    CHECK(s.Tick(0,0).empty()); // prevent duplicate load while in-flight
    CHECK(s.CompleteLoad(commands[0].Key,true,12*1024*1024));
    CHECK(!s.CompleteLoad(commands[0].Key,true)); // prevent duplicate acknowledgment
    CHECK(s.Stats().Resident==1);
    CHECK(s.Stats().ReservedBytes==20*1024*1024);
    CHECK(!s.CompleteUnload(Titan::Cell(900,900),true));
}
void TestStreamingEvictionAck() {
    auto c=SmallConfig(); c.MaxTiles=1; c.MaxResidentBytes=8*1024*1024;
    c.MaxInFlightLoads=1; c.MaxLoadsPerTick=1;
    Titan::WorldScheduler s(c);
    auto initial=s.Tick(-1.0,-101.0);
    CHECK(initial.size()==1 && initial[0].Key==Titan::Cell(-1,-2));
    CHECK(s.CompleteLoad(initial[0].Key,true));
    auto commands=s.Tick(10000,0);
    CHECK(commands.size()==1 && commands[0].Operation==Titan::StreamOp::Unload);
    CHECK(s.Stats().Unloading==1 && s.Stats().ReservedBytes==8*1024*1024);
    CHECK(s.Tick(10000,0).empty()); // No reuse until actual I/O complete
    CHECK(s.CompleteUnload(initial[0].Key,false)); // failed unload becomes resident
    CHECK(s.State(initial[0].Key)==Titan::TileState::Resident);
    commands=s.Tick(10000,0);
    CHECK(commands.size()==1 && commands[0].Operation==Titan::StreamOp::Unload);
    CHECK(s.CompleteUnload(initial[0].Key,true));
    CHECK(s.Stats().ReservedBytes==0);
    commands=s.Tick(10000,0);
    CHECK(commands.size()==1 && commands[0].Operation==Titan::StreamOp::Load);
}
void TestStreamingBudget() {
    auto c=SmallConfig();
    Titan::WorldScheduler s(c);
    auto commands=s.Tick(0,0);
    CHECK(commands.size()==2);
    CHECK(s.CompleteLoad(commands[0].Key,true,100*1024*1024));
    CHECK(s.CompleteLoad(commands[1].Key,false));
    auto more=s.Tick(0,0);
    CHECK(!more.empty() && more[0].Operation==Titan::StreamOp::Unload);
    CHECK(s.Stats().ReservedBytes >= 100*1024*1024);
    CHECK(s.CompleteUnload(commands[0].Key,true));
    more=s.Tick(0,0);
    CHECK(!more.empty() && more[0].Operation==Titan::StreamOp::Load);
    CHECK(s.Stats().TotalRecords<=c.MaxTiles);
}
void TestPredictionAndValidation() {
    auto c=SmallConfig(); c.MaxLoadsPerTick=1; c.MaxInFlightLoads=1;
    Titan::WorldScheduler s(c);
    CHECK(s.Tick(0,0,200,0)[0].Key.X >= 0);
    CHECK(s.Tick(std::numeric_limits<double>::quiet_NaN(),0).empty());
    CHECK(s.Tick(1e20,0).empty());
    MustThrow([&] { c.LoadRadius=128; s.SetConfig(c); });
    MustThrow([&] { c.LoadRadius=1; c.EstimatedTileBytes=0; s.SetConfig(c); });
}
void TestRandomStreamingInvariants() {
    auto c=SmallConfig(); c.MaxTiles=12; c.MaxResidentBytes=12*8*1024*1024;
    Titan::WorldScheduler s(c);
    for (int i=0;i<500;++i) {
        const double x=static_cast<double>((i*37)%29-14)*90.0;
        const double y=static_cast<double>((i*11)%23-11)*110.0;
        for (auto& cmd:s.Tick(x,y,100.0,-60.0)) {
            if (cmd.Operation==Titan::StreamOp::Load) CHECK(s.CompleteLoad(cmd.Key,true));
            else CHECK(s.CompleteUnload(cmd.Key,true));
        }
        auto st=s.Stats();
        CHECK(st.TotalRecords<=c.MaxTiles);
        CHECK(st.ReservedBytes<=c.MaxResidentBytes);
        CHECK(st.Loading==0 && st.Unloading==0);
    }
}
void TestFrameGraphDependencies() {
    Titan::FrameGraph g;
    int depth=g.AddResource("depth",1024);
    int gbuf=g.AddResource("gbuffer",4096);
    int hdr=g.AddResource("hdr",8192);
    int swap=g.AddResource("backbuffer",8192,true);
    g.AddPass("depth_prepass",{}, {depth});
    g.AddPass("gbuffer",{depth},{gbuf});
    g.AddPass("lighting",{gbuf,depth},{hdr});
    g.AddPass("postprocess",{hdr},{swap});
    auto plan=g.Compile();
    CHECK(plan.PassOrder.size()==4 && plan.PassOrder[0]==0 && plan.PassOrder[3]==3);
    CHECK(plan.EdgeCount>=3);
    CHECK(plan.Lifetimes[3].PoolSlot==-1);
    CHECK(plan.TransientPoolBytes==1024+4096+8192);
}
void TestFrameGraphAliasing() {
    Titan::FrameGraph g;
    int a=g.AddResource("a",4096), b=g.AddResource("b",2048);
    int p0=g.AddPass("produceA",{}, {a});
    int p1=g.AddPass("useA",{a},{});
    int p2=g.AddPass("produceB",{}, {b},{p1});
    g.AddPass("useB",{b},{});
    auto plan=g.Compile();
    CHECK(plan.Lifetimes[0].Last<plan.Lifetimes[1].First);
    CHECK(plan.Lifetimes[0].PoolSlot==plan.Lifetimes[1].PoolSlot);
    CHECK(plan.TransientPoolBytes==4096);
    CHECK(plan.PassOrder.size()==4 && plan.PassOrder[0]==p0 && plan.PassOrder[2]==p2);
}
void TestFrameGraphValidation() {
    Titan::FrameGraph g;
    int r=g.AddResource("r",10);
    g.AddPass("badRead",{r},{});
    MustThrow([&]{g.Compile();});
    Titan::FrameGraph x;
    int res=x.AddResource("x",10);
    int a=x.AddPass("A",{}, {res},{1});
    int b=x.AddPass("B",{res},{});
    CHECK(a==0 && b==1);
    MustThrow([&]{x.Compile();});
    Titan::FrameGraph y;
    int re=y.AddResource("y",10);
    y.AddPass("samePass",{re},{re});
    MustThrow([&]{y.Compile();});
    Titan::FrameGraph z;
    z.AddResource("ok",10);
    MustThrow([&]{z.AddResource("ok",20);});
}
void TestTaskGraphHazards() {
    Titan::TaskGraph t;
    std::atomic<int> counter{0};
    int a=t.AddTask({"physics",{}, {"transform"},[&]{++counter;}});
    int b=t.AddTask({"audio",{}, {"audio"},[&]{++counter;}});
    int c=t.AddTask({"render",{"transform"},{"frame"},[&]{++counter;}});
    auto plan=t.Compile();
    CHECK(a==0 && b==1 && c==2);
    CHECK(plan.Waves.size()==2 && plan.Waves[0].size()==2);
    t.Run(plan,2);
    CHECK(counter==3);
}
void TestTaskGraphErrors() {
    Titan::TaskGraph t;
    int a=t.AddTask({"a",{}, {},[]{}});
    int b=t.AddTask({"b",{}, {},[]{}});
    t.AddDependency(a,b); t.AddDependency(b,a);
    MustThrow([&]{t.Compile();});
    Titan::TaskGraph x;
    x.AddTask({"a",{}, {},[]{throw std::runtime_error("task fail");}});
    MustThrow([&]{x.Run(x.Compile(),2);});
    MustThrow([&]{x.Run(x.Compile(),0);});
}
int main() {
    const std::vector<std::pair<const char*,std::function<void()>>> tests = {
        {"stream_initialization",TestStreamInitialization},
        {"stream_eviction_ack",TestStreamingEvictionAck},
        {"stream_budget",TestStreamingBudget},
        {"stream_prediction_validation",TestPredictionAndValidation},
        {"stream_invariants_500_ticks",TestRandomStreamingInvariants},
        {"frame_graph_dependencies",TestFrameGraphDependencies},
        {"frame_graph_aliasing",TestFrameGraphAliasing},
        {"frame_graph_validation",TestFrameGraphValidation},
        {"task_graph_hazards",TestTaskGraphHazards},
        {"task_graph_errors",TestTaskGraphErrors},
    };
    for (const auto& test: tests) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch(const std::exception& e) { std::cerr << "FAIL " << test.first << ": " << e.what() << '\n'; return 1; }
    }
    std::cout << "TITAN: " << tests.size() << " test groups passed\n";
}
