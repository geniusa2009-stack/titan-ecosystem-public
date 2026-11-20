#include "TitanECS.h"
#include "TitanVisibility.h"
#include <chrono>
#include <iostream>
#include <vector>
using namespace Titan;
int main() {
    constexpr int N = 100000;
    EntityWorld world;
    std::vector<RenderObject> scene;
    scene.reserve(N);
    for (int i=0; i<N; ++i) {
        auto e = world.Create();
        world.Set(e, Transform{{double(i%400),double((i/400)%100),double(i/40000)*40+5}});
        world.Set(e, Velocity{{1.0,0,0}});
        scene.push_back({static_cast<uint64_t>(i+1),{double((i%400)-200),double((i/400)%100)-50,double(i/40000)*40+5},1.0,static_cast<uint32_t>(i%8),static_cast<uint32_t>(i%5)});
    }
    auto start=std::chrono::steady_clock::now();
    for (int tick=0; tick<100; ++tick)
        world.Each<Transform,Velocity>([](Entity, Transform& t, Velocity& v) {
            t.Position.X += v.Linear.X/60.0;
        });
    auto end=std::chrono::steady_clock::now();
    auto draw=BuildDrawList(Camera(),scene);
    double checkSum = 0.0;
    world.Each<Transform>([&checkSum](Entity, Transform& transform) { checkSum += transform.Position.X; });
    std::chrono::duration<double,std::milli> ms=end-start;
    std::cout << "position_checksum=" << checkSum << " " << "entities=" << world.AliveCount() << " steps=100 ecs_ms=" << ms.count()
              << " visible=" << draw.Items.size() << " culled=" << draw.Culled
              << " batches=" << draw.Batches.size() << "\n";
}
