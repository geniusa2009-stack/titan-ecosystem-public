#include "TitanECS.h"
#include "TitanFixedClock.h"
#include "TitanVisibility.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

using namespace Titan;
struct Health { int HitPoints = 0; };

void TestGenerations() {
    EntityWorld w;
    Entity first = w.Create();
    assert(w.Alive(first));
    w.Set(first, Health{55});
    assert(w.Get<Health>(first)->HitPoints == 55);
    assert(w.Destroy(first));
    assert(!w.Alive(first) && !w.Get<Health>(first));
    assert(!w.Destroy(first));
    Entity second = w.Create();
    assert(second.Index == first.Index);
    assert(second.Generation != first.Generation);
    assert(!w.Get<Health>(second));
    try { w.Set(first, Health{2}); assert(false); } catch (const std::invalid_argument&) {}
}
void TestPoolCompaction() {
    EntityWorld w;
    std::vector<Entity> ids;
    for (int i = 0; i < 1000; ++i) {
        Entity e = w.Create(); ids.push_back(e); w.Set(e, Health{i});
    }
    for (int i = 0; i < 1000; i += 3) assert(w.Destroy(ids[i]));
    int sum = 0, count = 0;
    w.Each<Health>([&](Entity e, Health& h) {
        assert(w.Alive(e));
        assert(h.HitPoints % 3 != 0);
        sum += h.HitPoints; ++count;
    });
    assert(count == 666 && sum > 0);
    for (int i = 1; i < 1000; ++i)
        if (i % 3 != 0) assert(w.Get<Health>(ids[i])->HitPoints == i);
}
void TestTwoComponentJoin() {
    EntityWorld w;
    std::vector<Entity> ids;
    for (int i = 0; i < 99; ++i) {
        Entity e = w.Create(); ids.push_back(e);
        w.Set(e, Health{i});
        if (i % 10 == 0) w.Set(e, Velocity{{1, 2, 3}});
    }
    int count = 0;
    w.Each<Health, Velocity>([&](Entity, Health& h, Velocity& v) { h.HitPoints += int(v.Linear.Y); ++count; });
    assert(count == 10);
    w.Each<Velocity, Health>([&](Entity, Velocity&, Health&) { ++count; });
    assert(count == 20);
    assert(w.Get<Health>(ids[0])->HitPoints == 2);
    assert(w.Remove<Health>(ids[0]));
    assert(!w.Remove<Health>(ids[0]));
}
void TestDeferredAndMutationGuard() {
    EntityWorld w; DeferredCommands cmd;
    Entity e = w.Create(); w.Set(e, Health{1});
    bool threw = false;
    w.Each<Health>([&](Entity id, Health&) {
        cmd.Destroy(id);
        try { w.Destroy(id); } catch (const std::logic_error&) { threw = true; }
    });
    assert(threw && w.Alive(e) && cmd.Pending() == 1);
    cmd.Apply(w);
    assert(!w.Alive(e) && cmd.Pending() == 0);
    Entity fresh = w.Create();
    cmd.Set(fresh, Health{300}); cmd.Apply(w);
    assert(w.Get<Health>(fresh)->HitPoints == 300);
    cmd.Remove<Health>(fresh); cmd.Apply(w);
    assert(!w.Get<Health>(fresh));
}
void TestFixedClock() {
    FixedClock clock(0.01, 4, 0.1);
    int count = 0;
    for (int i = 0; i < 10; ++i)
        clock.Advance(0.003, [&](double step) { assert(std::abs(step-0.01)<1e-9); ++count; });
    assert(count == 3);
    auto overloaded = clock.Advance(2.0, [&](double) { ++count; });
    assert(overloaded.Steps == 4 && overloaded.DroppedSeconds > 1.9);
    assert(overloaded.Interpolation >= 0 && overloaded.Interpolation <= 1);
    bool threw = false;
    try { clock.Advance(-1, [&](double) {}); } catch (const std::invalid_argument&) { threw = true; }
    assert(threw);
}
void TestVisibleAndBatch() {
    Camera c;
    std::vector<RenderObject> objects = {
        {1,{0,0,10},1,7,8}, {2,{0,0,11},1,7,8},
        {3,{0,0,-20},1,7,8}, {4,{1000,0,10},1,7,8},
        {5,{0,0,2000},1,7,8}, {6,{0,0,10},1,7,9},
        {7,{0,0,30},1,7,8}, {8,{0,0,200},1,7,8},
    };
    auto list = BuildDrawList(c, objects);
    assert(list.Items.size() == 5);
    assert(list.Culled == 3);
    assert(list.Batches.size() >= 2);
    std::map<uint64_t,int> lod;
    for (auto item : list.Items) lod[item.Id] = item.Lod;
    assert(lod[1] == 0 && lod[7] == 1 && lod[8] == 2);
    std::size_t batchesSum = 0;
    for (auto batch : list.Batches) batchesSum += batch.Count;
    assert(batchesSum == list.Items.size());
}
void TestNearPlaneSphereAndInvalid() {
    Camera c;
    std::vector<RenderObject> objects = {{1,{0,0,0.05},0.2,1,1}, {2,{0,0,100},-1,1,1}};
    auto list = BuildDrawList(c, objects);
    assert(list.Items.size()==1 && list.Invalid==1);
    bool threw = false;
    c.Far = 0.01;
    try { BuildDrawList(c, objects); } catch (const std::invalid_argument&) { threw=true; }
    assert(threw);
}
void TestStress() {
    EntityWorld w;
    std::vector<Entity> all;
    for (int i=0; i<30000; ++i) {
        Entity e=w.Create(); all.push_back(e);
        w.Set(e, Transform{{double(i),0,0}});
        w.Set(e, Velocity{{1,0,0}});
    }
    w.Each<Transform, Velocity>([](Entity, Transform& t, Velocity& v) { t.Position.X += v.Linear.X * (1.0/60); });
    assert(w.AliveCount() == all.size());
    for (std::size_t i=0; i<all.size(); i+=2) w.Destroy(all[i]);
    assert(w.AliveCount() == 15000);
    for (std::size_t i=1; i<all.size(); i+=2)
        assert(std::abs(w.Get<Transform>(all[i])->Position.X - (double(i) + 1.0/60)) < 1e-8);
}
int main() {
    TestGenerations(); TestPoolCompaction(); TestTwoComponentJoin();
    TestDeferredAndMutationGuard(); TestFixedClock(); TestVisibleAndBatch();
    TestNearPlaneSphereAndInvalid(); TestStress();
    std::cout << "TITAN DATA: 8 test suites passed\n";
}
