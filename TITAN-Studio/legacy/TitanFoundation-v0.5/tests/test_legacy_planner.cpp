#include "TitanSpatialPlanner.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>
using namespace TitanSpatial;

int main()
{
    Config C;
    C.TileWorldSize = 100.0;
    C.LoadRadius = 1;
    C.UnloadRadius = 2;
    C.MaxActive = 5;
    C.MaxLoadsPerUpdate = 2;
    C.MaxUnloadsPerUpdate = 5;
    PlanResult R = Plan(C, 0, 0, std::vector<Tile>());
    assert(R.Center == Tile(0, 0));
    assert(R.ToLoad.size() == 2);
    assert(R.ToLoad[0] == Tile(0, 0)); // closest first
    assert(R.ToUnload.empty());

    // Negative positions must floor correctly (not truncate).
    assert(Plan(C, -1.0, -101.0, {}).Center == Tile(-1, -2));
    // Unload outside hysteresis radius.
    R = Plan(C, 0, 0, { Tile(0, 0), Tile(8, 8), Tile(-7, 1) });
    assert(R.ToUnload.size() == 2);
    assert(R.ToLoad.size() == 2);
    // Budget pressure: do not admit new tiles beyond capacity.
    C.MaxActive = 1;
    C.MaxUnloadsPerUpdate = 0;
    R = Plan(C, 0, 0, { Tile(10, 10) });
    assert(R.ToLoad.empty());
    // When capacity is freed in this tick, it can be reused.
    C.MaxUnloadsPerUpdate = 1;
    R = Plan(C, 0, 0, { Tile(10, 10) });
    assert(R.ToUnload.size() == 1 && R.ToLoad.size() == 1);
    // Tile enumeration stays deterministic.
    const PlanResult A = Plan(C, 0, 0, {});
    const PlanResult B = Plan(C, 0, 0, {});
    assert(A.ToLoad == B.ToLoad);
    // Invalid coordinates do nothing instead of producing undefined behavior.
    assert(Plan(C, std::nan(""), 0, {}).ToLoad.empty());
    C.TileWorldSize = 0.0;
    assert(Plan(C, 0, 0, {}).ToLoad.empty());

    std::cout << "TITAN spatial planner: all checks passed\n";
}
