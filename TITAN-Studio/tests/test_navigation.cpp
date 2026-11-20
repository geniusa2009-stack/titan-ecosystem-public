// TITAN navigation core tests: validation, A* optimality against Dijkstra, determinism, partial paths, clearance,
// dynamic re-planning, smoothing safety and serialization. Original TITAN code.
#include "titan/navigation.h"
#include "titan_test.h"

#include <functional>
#include <limits>
#include <queue>

using namespace titan;
using namespace titan::nav;

namespace {
const double kNan = std::numeric_limits<double>::quiet_NaN();

struct Rng {
    uint64_t s;
    double next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return double(s >> 11) * (1.0 / 9007199254740992.0);
    }
    double range(double a, double b) { return a + (b - a) * next(); }
    size_t pick(size_t n) { return size_t(next() * double(n)) % n; }
};

// Reference: plain Dijkstra over the same neighbour generator; returns the least cost or +inf.
double dijkstra(const NavSpace& s, Index start, Index goal, const AgentProfile& p) {
    if (!s.valid(start) || !s.valid(goal) || !s.passable(start, p) || !s.passable(goal, p)) return INFINITY;
    std::vector<double> dist(s.capacity(), INFINITY);
    using Item = std::pair<double, Index>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
    dist[start] = 0;
    q.push({0, start});
    std::vector<Step> steps;
    while (!q.empty()) {
        auto [d, i] = q.top();
        q.pop();
        if (d > dist[i]) continue;
        if (i == goal) return d;
        s.expand(i, p, steps);
        for (const Step& st : steps)
            if (d + st.cost < dist[st.to]) {
                dist[st.to] = d + st.cost;
                q.push({dist[st.to], st.to});
            }
    }
    return INFINITY;
}

// The path must be a chain of real steps whose costs sum to the reported cost.
bool validPath(const NavSpace& s, const PathResult& r, const AgentProfile& p) {
    double sum = 0;
    std::vector<Step> steps;
    for (size_t k = 1; k < r.path.size(); ++k) {
        s.expand(r.path[k - 1], p, steps);
        double best = INFINITY;  // parallel edges: the search uses the cheapest
        for (const Step& st : steps)
            if (st.to == r.path[k]) best = std::min(best, st.cost);
        if (std::isinf(best)) return false;
        sum += best;
    }
    return std::fabs(sum - r.cost) <= 1e-9 * (1 + r.cost);
}

bool near(double a, double b) { return std::fabs(a - b) <= 1e-9 * (1 + std::fabs(b)); }
}  // namespace

TEST("nav: invalid profiles, budgets, starts and goals are reported, not thrown") {
    NavGraph g;
    const NodeId a = g.addNode({0, 0, 0}), b = g.addNode({1, 0, 0});
    g.addEdge(a, b, 1);
    SearchContext ctx;
    const Index ia = *g.index(a), ib = *g.index(b);
    AgentProfile bad;
    bad.radius = kNan;
    CHECK(findPath(g, ia, ib, bad, {}, ctx).status == PathStatus::InvalidRequest);
    bad = {};
    bad.areaCost[3] = 0.5;
    const PathResult cheap = findPath(g, ia, ib, bad, {}, ctx);
    CHECK(cheap.status == PathStatus::InvalidRequest && !cheap.reason.empty() && cheap.path.empty());
    SearchBudget zero;
    zero.maxExpansions = 0;
    CHECK(findPath(g, ia, ib, {}, zero, ctx).status == PathStatus::InvalidRequest);
    CHECK(findPath(g, 99, ib, {}, {}, ctx).status == PathStatus::InvalidStart);
    CHECK(findPath(g, ia, kNoIndex, {}, {}, ctx).status == PathStatus::InvalidGoal);
    g.setNodeBlocked(b, true);
    CHECK(findPath(g, ia, ib, {}, {}, ctx).status == PathStatus::InvalidGoal);
    AgentProfile wide;
    wide.radius = 2000;  // invalid: above the 1000 limit
    CHECK_THROWS(wide.validate());
    AgentProfile p;
    p.radius = 0.75;
    p.allowedAreas = 0x8000000000000005ull;
    p.areaCost[63] = 12.5;
    p.allowDiagonal = false;
    const AgentProfile back = AgentProfile::fromJson(Json::parse(p.toJson().dump()));
    CHECK(back.toJson() == p.toJson() && back.allowedAreas == p.allowedAreas);
    Json badMask = p.toJson();
    badMask["allowedAreas"] = "xyz";
    CHECK_THROWS(AgentProfile::fromJson(badMask));
}

TEST("nav graph: invalid edits are refused without side effects") {
    GraphSettings s;
    s.heuristicScale = 1;
    s.maxNodes = 3;
    NavGraph g(s);
    const NodeId a = g.addNode({0, 0, 0}), b = g.addNode({10, 0, 0});
    const uint64_t rev = g.revision();
    CHECK_THROWS(g.addEdge(a, b, -1));
    CHECK_THROWS(g.addEdge(a, b, kNan));
    CHECK_THROWS(g.addEdge(a, b, INFINITY));
    CHECK_THROWS(g.addEdge(a, b, 5));    // cheaper than heuristicScale * distance: inadmissible
    CHECK_THROWS(g.addEdge(a, 999, 20));
    CHECK_THROWS(g.addEdge(a, a, 1));
    CHECK_THROWS(g.addEdge(a, b, 20, 64));
    CHECK_THROWS(g.addEdge(a, b, 20, 0, kNan));
    CHECK_THROWS(g.addNode({kNan, 0, 0}));
    CHECK_THROWS(g.addNode({2e12, 0, 0}));
    CHECK_THROWS(g.addNode({0, 0, 0}, 64));
    CHECK(g.edgeCount() == 0 && g.nodeCount() == 2 && g.revision() == rev);
    const EdgeId e1 = g.addEdge(a, b, 10);
    CHECK_THROWS(g.setEdgeCost(e1, 9));
    CHECK(g.edge(e1)->cost == 10);
    const NodeId c = g.addNode({5, 5, 0});
    CHECK_THROWS(g.addNode({1, 1, 1}));  // node limit
    g.addEdge(b, c, 20);
    g.addEdge(c, a, 20);
    CHECK(g.removeNode(c));
    CHECK(g.edgeCount() == 1 && !g.node(c) && g.edge(e1));
    CHECK(!g.removeNode(c) && !g.removeEdge(12345));
    const NodeId d = g.addNode({1, 1, 1});  // reuses the freed slot, but never the id
    CHECK(d != c && g.index(d).has_value());
}

TEST("nav graph: A* matches Dijkstra on random graphs with areas, blocking and radius limits") {
    GraphSettings s;
    s.heuristicScale = 1;
    NavGraph g(s);
    Rng rng{7};
    std::vector<NodeId> nodes;
    for (int i = 0; i < 250; ++i) nodes.push_back(g.addNode({rng.range(0, 100), rng.range(0, 100), 0}, uint8_t(rng.pick(2))));
    for (int i = 0; i < 1300; ++i) {
        const NodeId a = nodes[rng.pick(nodes.size())], b = nodes[rng.pick(nodes.size())];
        if (a == b) continue;
        const NavNode& na = *g.node(a);
        const NavNode& nb = *g.node(b);
        const double dx = na.position.x - nb.position.x, dy = na.position.y - nb.position.y;
        const double cost = std::sqrt(dx * dx + dy * dy) * rng.range(1, 3);
        const EdgeId e = g.addEdge(a, b, cost, uint8_t(rng.pick(4)), rng.next() < 0.1 ? 0.2 : 1000);
        if (rng.next() < 0.05) g.setEdgeBlocked(e, true);
    }
    for (int i = 0; i < 10; ++i) g.setNodeBlocked(nodes[rng.pick(nodes.size())], true);
    nodes.push_back(g.addNode({50, 50, 0}));  // isolated: guarantees unreachable queries
    AgentProfile p;
    p.radius = 0.5;
    p.areaCost[1] = 2;
    p.areaCost[2] = 5;
    p.allowedAreas = ~(1ull << 3);
    SearchContext ctx;
    int found = 0, missing = 0;
    for (int q = 0; q < 150; ++q) {
        const Index a = *g.index(nodes[rng.pick(nodes.size())]);
        const Index b = q % 10 == 0 ? *g.index(nodes.back()) : *g.index(nodes[rng.pick(nodes.size())]);
        const double ref = dijkstra(g, a, b, p);
        const PathResult r = findPath(g, a, b, p, {}, ctx);
        if (!g.passable(a, p) || !g.passable(b, p)) {
            CHECK(r.status == PathStatus::InvalidStart || r.status == PathStatus::InvalidGoal);
            continue;
        }
        if (std::isinf(ref)) {
            CHECK(r.status == PathStatus::NoPath && r.path.empty());
            ++missing;
            continue;
        }
        CHECK(r.status == PathStatus::Found);
        CHECK(near(r.cost, ref));
        CHECK(r.path.front() == a && r.path.back() == b && validPath(g, r, p));
        const PathResult again = findPath(g, a, b, p, {}, ctx);
        CHECK(again.path == r.path && again.cost == r.cost);  // deterministic, context reuse is clean
        ++found;
    }
    CHECK(found > 20 && missing > 0);
}

TEST("nav graph: equal-cost alternatives resolve to the same, lowest-index route") {
    auto build = [](bool reversedEdges) {
        NavGraph g;
        const NodeId a = g.addNode({0, 0, 0}), b = g.addNode({1, 1, 0}), c = g.addNode({1, -1, 0}), d = g.addNode({2, 0, 0});
        if (reversedEdges) {
            g.addEdge(c, d, 1);
            g.addEdge(a, c, 1);
            g.addEdge(b, d, 1);
            g.addEdge(a, b, 1);
        } else {
            g.addEdge(a, b, 1);
            g.addEdge(b, d, 1);
            g.addEdge(a, c, 1);
            g.addEdge(c, d, 1);
        }
        return g;
    };
    SearchContext ctx;
    for (bool rev : {false, true}) {
        const NavGraph g = build(rev);
        const PathResult r = findPath(g, *g.index(1), *g.index(4), {}, {}, ctx);
        const std::vector<NodeId> want{1, 2, 4};
        CHECK(r.status == PathStatus::Found && g.ids(r.path) == want && r.cost == 2);
    }
}

TEST("nav: partial paths, expansion budgets and path length limits") {
    GraphSettings s;
    s.heuristicScale = 1;
    NavGraph g(s);
    for (int i = 0; i < 4; ++i) g.addNode({double(i), 0, 0});
    const NodeId goal = g.addNode({10, 0, 0});  // isolated
    g.addEdge(1, 2, 1);
    g.addEdge(2, 3, 1);
    g.addEdge(3, 4, 1);
    SearchContext ctx;
    const Index s1 = *g.index(1), gi = *g.index(goal);
    CHECK(findPath(g, s1, gi, {}, {}, ctx).status == PathStatus::NoPath);
    const PathResult part = findPath(g, s1, gi, {}, {}, ctx, true);
    const std::vector<NodeId> chain{1, 2, 3, 4};
    CHECK(part.status == PathStatus::Partial && g.ids(part.path) == chain && part.cost == 3 && !part.reason.empty());
    SearchBudget tight;
    tight.maxExpansions = 2;
    const PathResult capped = findPath(g, s1, gi, {}, tight, ctx);
    CHECK(capped.status == PathStatus::BudgetExceeded && capped.path.empty() && capped.expansions == 2);
    const PathResult cappedPartial = findPath(g, s1, gi, {}, tight, ctx, true);
    const std::vector<NodeId> two{1, 2};
    CHECK(cappedPartial.status == PathStatus::Partial && g.ids(cappedPartial.path) == two);
    SearchBudget shortPaths;
    shortPaths.maxPathNodes = 3;
    CHECK(findPath(g, s1, *g.index(4), {}, shortPaths, ctx).status == PathStatus::BudgetExceeded);
    const PathResult self = findPath(g, s1, s1, {}, {}, ctx);
    CHECK(self.status == PathStatus::Found && self.path.size() == 1 && self.cost == 0);
}

TEST("nav grid: A* matches Dijkstra with costs, areas, clearance and both neighbourhoods") {
    NavGrid grid(48, 32, 0.5);
    Rng rng{99};
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 48; ++x) {
            CellInfo c;
            c.cost = float(1 + rng.pick(5));
            c.area = uint8_t(rng.pick(3));
            c.blocked = rng.next() < 0.12;
            grid.setCell(x, y, c);
        }
    AgentProfile thin;
    thin.radius = 0;
    thin.areaCost[2] = 3;
    AgentProfile wide = thin;
    wide.radius = 0.6;  // one cell of clearance at 0.5 m cells
    AgentProfile rook = thin;
    rook.allowDiagonal = false;
    rook.allowedAreas = ~(1ull << 1);
    SearchContext ctx;
    int found = 0;
    for (const AgentProfile* p : {&thin, &wide, &rook}) {
        auto passableCell = [&] {  // sample endpoints the agent can stand on (bounded tries)
            Index i = 0;
            for (int t = 0; t < 1000; ++t)
                if (grid.passable(i = Index(rng.pick(grid.capacity())), *p)) break;
            return i;
        };
        for (int q = 0; q < 80; ++q) {
            const Index a = q % 8 == 0 ? Index(rng.pick(grid.capacity())) : passableCell();
            const Index b = passableCell();
            const PathResult r = findPath(grid, a, b, *p, {}, ctx);
            if (!grid.passable(a, *p) || !grid.passable(b, *p)) {
                CHECK(!r.ok());
                continue;
            }
            const double ref = dijkstra(grid, a, b, *p);
            if (std::isinf(ref)) {
                CHECK(r.status == PathStatus::NoPath);
                continue;
            }
            CHECK(r.status == PathStatus::Found && near(r.cost, ref) && validPath(grid, r, *p));
            for (size_t k = 1; k < r.path.size(); ++k) {  // no corner cutting on diagonal steps
                int x0, y0, x1, y1;
                grid.coords(r.path[k - 1], x0, y0);
                grid.coords(r.path[k], x1, y1);
                CHECK(std::abs(x1 - x0) <= 1 && std::abs(y1 - y0) <= 1);
                if (x0 != x1 && y0 != y1) {
                    CHECK(p->allowDiagonal);
                    CHECK(grid.passable(grid.index(x1, y0), *p) && grid.passable(grid.index(x0, y1), *p));
                }
            }
            ++found;
        }
    }
    CHECK(found > 40);
}

TEST("nav grid: clearance, borders and invalid edits") {
    NavGrid unit(7, 7, 1.0);
    CHECK(unit.requiredClearance(0) == 0 && unit.requiredClearance(0.5) == 0 && unit.requiredClearance(0.51) == 1);
    CHECK(unit.requiredClearance(1.5) == 1 && unit.requiredClearance(1.51) == 2);
    CellInfo wall;
    wall.blocked = true;
    unit.fillRect(0, 0, 6, 6, wall);
    unit.fillRect(0, 3, 6, 3, CellInfo{});  // a one-cell-wide corridor
    AgentProfile slim, broad;
    slim.radius = 0.4;
    broad.radius = 0.6;
    SearchContext ctx;
    CHECK(findPath(unit, unit.index(0, 3), unit.index(6, 3), slim, {}, ctx).status == PathStatus::Found);
    CHECK(findPath(unit, unit.index(0, 3), unit.index(6, 3), broad, {}, ctx).status == PathStatus::InvalidStart);
    NavGrid open(5, 5, 1.0);
    CHECK(!open.passable(open.index(0, 0), broad) && open.passable(open.index(1, 1), broad));  // grid edge counts as wall
    const uint64_t rev = open.revision();
    CellInfo bad;
    bad.cost = 0;
    CHECK_THROWS(open.setCell(1, 1, bad));
    bad.cost = std::numeric_limits<float>::infinity();
    CHECK_THROWS(open.setCell(1, 1, bad));
    bad = {};
    bad.area = 64;
    CHECK_THROWS(open.setCell(1, 1, bad));
    CHECK_THROWS(open.fillRect(0, 0, 5, 0, wall));  // x1 out of range: nothing applied
    CHECK_THROWS(open.fillRect(3, 0, 1, 0, wall));
    CHECK(open.revision() == rev && !open.cell(0, 0).blocked);
    CHECK_THROWS(NavGrid(0, 5, 1.0));
    CHECK_THROWS(NavGrid(5, 5, kNan));
    GridSettings small;
    small.maxCells = 10;
    CHECK_THROWS(NavGrid(4, 4, 1.0, 0, 0, small));
    CHECK(!open.cellAt(kNan, 0) && !open.cellAt(-0.01, 0) && open.cellAt(4.99, 4.99) == open.index(4, 4));
}

TEST("nav planner: dynamic obstacles trigger re-planning") {
    NavGrid grid(20, 10, 1.0);
    PlannerSettings ps;
    NavPlanner planner(grid, ps);
    AgentProfile p;
    p.radius = 0;
    const AgentId a = planner.addAgent(p);
    planner.request(a, grid.index(0, 5), grid.index(19, 5));
    CHECK(planner.update() == 1);
    CHECK(planner.agent(a)->result.status == PathStatus::Found && planner.agent(a)->result.path.size() == 20);
    CellInfo wall;
    wall.blocked = true;
    grid.fillRect(10, 0, 10, 8, wall);  // leaves a single gap at (10, 9)
    CHECK(planner.update() == 1);
    const AgentState& st = *planner.agent(a);
    CHECK(st.replans == 1 && st.result.status == PathStatus::Found);
    const auto& path = st.result.path;
    CHECK(std::find(path.begin(), path.end(), grid.index(10, 9)) != path.end());
    grid.setCell(10, 9, wall);
    planner.update();
    CHECK(planner.agent(a)->result.status == PathStatus::Partial);  // the planner allows partial paths by default
    CHECK(planner.update() == 0);  // nothing changed: no re-plan
}

TEST("nav planner: several agents, per-update budget, FIFO order and stale ids") {
    NavGraph g;
    const NodeId n1 = g.addNode({0, 0, 0}), n2 = g.addNode({1, 0, 0}), n3 = g.addNode({2, 0, 0}), n4 = g.addNode({1, 1, 0});
    g.addEdge(n1, n2, 1, 0, 0.3);  // narrow route
    g.addEdge(n2, n3, 1, 0, 0.3);
    g.addEdge(n1, n4, 1.5);        // wide route
    g.addEdge(n4, n3, 1.5);
    PlannerSettings ps;
    ps.maxRequestsPerUpdate = 2;
    ps.maxAgents = 3;
    NavPlanner planner(g, ps);
    AgentProfile small, big;
    small.radius = 0.2;
    big.radius = 0.5;
    const AgentId a = planner.addAgent(small), b = planner.addAgent(big), c = planner.addAgent(small);
    CHECK_THROWS(planner.addAgent(small));  // agent limit
    CHECK_THROWS(planner.request(999, 0, 0));
    const Index i1 = *g.index(n1), i3 = *g.index(n3);
    planner.request(a, i1, i3);
    planner.request(b, i1, i3);
    planner.request(c, 77, i3);
    CHECK(planner.update() == 2 && planner.pending() == 1);
    CHECK(planner.agent(c)->state == AgentPathState::Pending);
    const std::vector<NodeId> narrow{n1, n2, n3}, broadRoute{n1, n4, n3};
    CHECK(g.ids(planner.agent(a)->result.path) == narrow && planner.agent(a)->result.cost == 2);
    CHECK(g.ids(planner.agent(b)->result.path) == broadRoute && planner.agent(b)->result.cost == 3);
    CHECK(planner.update() == 1 && planner.agent(c)->result.status == PathStatus::InvalidStart);
    g.removeNode(n3);
    planner.update();
    planner.update();
    CHECK(planner.agent(a)->result.status == PathStatus::InvalidGoal && planner.agent(b)->result.status == PathStatus::InvalidGoal);
    CHECK(planner.removeAgent(b) && !planner.agent(b) && !planner.removeAgent(b));
}

TEST("nav grid: smoothing removes corners only where it is safe and stays within budget") {
    AgentProfile p;
    p.radius = 0;
    SearchContext ctx;
    auto segmentsClear = [](const NavGrid& g, const std::vector<DVec3>& pts, double maxCost) {
        for (size_t k = 1; k < pts.size(); ++k)
            for (int s = 0; s <= 2000; ++s) {
                const double t = s / 2000.0;
                const auto c = g.cellAt(pts[k - 1].x + (pts[k].x - pts[k - 1].x) * t, pts[k - 1].z + (pts[k].z - pts[k - 1].z) * t);
                if (!c) return false;
                int x, y;
                g.coords(*c, x, y);
                if (g.cell(x, y).blocked || g.cell(x, y).cost > maxCost) return false;
            }
        return true;
    };
    NavGrid open(20, 20, 1.0);
    const PathResult straight = findPath(open, open.index(0, 0), open.index(10, 4), p, {}, ctx);
    const std::vector<DVec3> line = open.smooth(straight.path, p);
    CHECK(straight.path.size() > 2 && line.size() == 2);
    CHECK(open.smooth(straight.path, p, 0).size() == straight.path.size());  // no budget: unchanged

    NavGrid walled(20, 20, 1.0);
    CellInfo wall;
    wall.blocked = true;
    walled.fillRect(4, 0, 5, 6, wall);
    const PathResult around = findPath(walled, walled.index(0, 2), walled.index(10, 2), p, {}, ctx);
    const std::vector<DVec3> pts = walled.smooth(around.path, p);
    CHECK(around.status == PathStatus::Found && pts.size() >= 3 && pts.size() < around.path.size());
    CHECK(segmentsClear(walled, pts, 1));

    NavGrid strip(20, 11, 1.0);
    CellInfo mud;
    mud.cost = 50;
    strip.fillRect(8, 0, 11, 9, mud);
    const PathResult detour = findPath(strip, strip.index(0, 5), strip.index(19, 5), p, {}, ctx);
    CHECK(detour.status == PathStatus::Found);
    const std::vector<DVec3> smoothed = strip.smooth(detour.path, p);
    CHECK(segmentsClear(strip, smoothed, 1));  // never cuts through the expensive strip the planner avoided
    CHECK_THROWS(open.smooth({kNoIndex}, p));
}

TEST("nav: graph and grid serialization round trips and refusals") {
    GraphSettings s;
    s.heuristicScale = 0.5;
    NavGraph g(s);
    const NodeId a = g.addNode({0, 0, 0}, 2, 3.5), b = g.addNode({4, 0, 0}), c = g.addNode({4, 4, 1});
    const EdgeId e = g.addEdge(a, b, 2.5, 1, 0.75);
    g.addEdge(b, c, 3);
    g.removeNode(c);
    g.setNodeBlocked(b, true);
    g.setEdgeBlocked(e, true);
    const Json j = g.toJson();
    const NavGraph back = NavGraph::fromJson(Json::parse(j.dump()));
    CHECK(back.toJson() == j);
    CHECK(back.node(b)->blocked && back.edge(e)->blocked && back.edge(e)->maxRadius == 0.75);
    NavGraph cont = NavGraph::fromJson(j);
    CHECK(cont.addNode({1, 1, 1}) == c + 1);  // ids are never reused after reload
    Json v2 = j;
    v2["version"] = 2;
    CHECK_THROWS(NavGraph::fromJson(v2));
    Json dangling = j;
    dangling["edges"].asArray()[0]["to"] = 99;
    CHECK_THROWS(NavGraph::fromJson(dangling));
    Json unordered = j;
    std::swap(unordered["nodes"].asArray()[0], unordered["nodes"].asArray()[1]);
    CHECK_THROWS(NavGraph::fromJson(unordered));
    Json staleNext = j;
    staleNext["nextNodeId"] = 1;
    CHECK_THROWS(NavGraph::fromJson(staleNext));
    Json cheap = j;
    cheap["edges"].asArray()[0]["cost"] = 1;  // below 0.5 * distance 4
    CHECK_THROWS(NavGraph::fromJson(cheap));

    NavGrid grid(6, 4, 0.25, -3, 7);
    CellInfo c1;
    c1.cost = 3;
    c1.area = 5;
    grid.fillRect(1, 1, 4, 2, c1);
    CellInfo wall;
    wall.blocked = true;
    grid.setCell(5, 3, wall);
    const Json gj = grid.toJson();
    const NavGrid gb = NavGrid::fromJson(Json::parse(gj.dump()));
    CHECK(gb.toJson() == gj && gb.cell(5, 3).blocked && gb.cell(2, 2) == c1);
    AgentProfile p;
    p.radius = 0.3;
    CHECK(gb.passable(gb.index(3, 1), p) == grid.passable(grid.index(3, 1), p));
    Json shortRuns = gj;
    shortRuns["cells"].asArray().pop_back();
    CHECK_THROWS(NavGrid::fromJson(shortRuns));
    Json longRuns = gj;
    longRuns["cells"].push_back(Json(Json::Array{Json(1), Json(1), Json(0), Json(false)}));
    CHECK_THROWS(NavGrid::fromJson(longRuns));
    Json zeroCost = gj;
    zeroCost["cells"].asArray()[0].asArray()[1] = 0;
    CHECK_THROWS(NavGrid::fromJson(zeroCost));
}

TITAN_TEST_MAIN()
