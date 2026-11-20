// TITAN Engine — navigation core: deterministic A* over graphs and grids, agent profiles, dynamic updates and a
// multi-agent path planner. Original TITAN code.
//
// Scope. Engine-independent navigation *model* and search, used by headless tools, tests and gameplay that does
// not need a navmesh. The Unreal integration adapts it to the Unreal Navigation System (Recast/Detour navmesh,
// nav areas and query filters), which stays the production navmesh generator; TITAN does not build navmeshes.
//
// Search contracts (findPath):
//  * Costs are finite and >= 0 everywhere; area multipliers are in [1, 1e6]. The heuristic never overestimates
//    and is consistent, so A* returns a least-cost path (tests compare against Dijkstra).
//  * Determinism: the open list is ordered by (f, h, index); neighbours are expanded in a fixed order; on equal
//    cost the parent with the lower index wins. The same query on the same data always returns the same path.
//  * Bounded: at most budget.maxExpansions node expansions and budget.maxPathNodes path nodes. Memory is one
//    SearchContext (O(capacity) scratch, reused across queries without reallocation).
//  * Results carry an explicit PathStatus and a human-readable reason. Partial paths (when requested) end at the
//    expanded node closest to the goal by the heuristic (ties: lower cost, then lower index).
//
// Dynamic updates: every mutation bumps NavSpace::revision(); NavPlanner re-plans agents whose path was computed
// at an older revision. Stable identity: indices may be reused after removal, so callers keep key(index), which
// is never reused, and the planner refuses requests whose start or goal key changed.
//
// Thread safety: spaces and planners are single-threaded for mutation. findPath() only reads the space; concurrent
// searches are safe with one SearchContext per thread and no concurrent mutation.
// Not exposed over the protocol yet: no capability is advertised until it is wired and verified.
#pragma once

#include "titan/json.h"
#include "titan/spatial.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace titan::nav {

using spatial::DVec3;
using Index = uint32_t;
constexpr Index kNoIndex = 0xFFFFFFFFu;
constexpr int kAreaCount = 64;

struct AgentProfile {
    double radius = 0.4;                // [0, 1000]
    uint64_t allowedAreas = ~0ull;      // bit a set: area a may be entered
    std::array<double, kAreaCount> areaCost;  // multipliers in [1, 1e6]
    bool allowDiagonal = true;          // grids: 8-neighbourhood without corner cutting

    AgentProfile() { areaCost.fill(1.0); }
    void validate() const;  // std::invalid_argument
    Json toJson() const;
    static AgentProfile fromJson(const Json& j);
};

struct SearchBudget {
    uint32_t maxExpansions = 65536;  // [1, 2^26]
    uint32_t maxPathNodes = 65536;   // [1, 2^26]
};

enum class PathStatus { Found, Partial, NoPath, BudgetExceeded, InvalidStart, InvalidGoal, InvalidRequest };
const char* toString(PathStatus s);

struct PathResult {
    PathStatus status = PathStatus::InvalidRequest;
    std::vector<Index> path;  // start .. goal (or .. closest node for Partial); empty on failure
    double cost = 0;
    uint32_t expansions = 0;
    std::string reason;
    bool ok() const { return status == PathStatus::Found || status == PathStatus::Partial; }
};

struct Step {
    Index to = kNoIndex;
    double cost = 0;
};

// Abstract search space. Indices are dense in [0, capacity()).
class NavSpace {
public:
    virtual ~NavSpace() = default;
    virtual size_t capacity() const = 0;
    virtual bool valid(Index i) const = 0;
    virtual uint64_t key(Index i) const = 0;  // stable, never-reused identity of a valid index
    virtual uint64_t revision() const = 0;
    virtual bool passable(Index i, const AgentProfile& p) const = 0;
    virtual double heuristic(Index from, Index goal, const AgentProfile& p) const = 0;
    virtual void expand(Index i, const AgentProfile& p, std::vector<Step>& out) const = 0;  // clears `out`
};

class SearchContext {
public:
    SearchContext() = default;

private:
    friend PathResult findPath(const NavSpace&, Index, Index, const AgentProfile&, const SearchBudget&, SearchContext&,
                               bool);
    struct Entry {
        double f, h, g;
        Index i;
    };
    void prepare(size_t n);
    bool seen(Index i) const { return stamp_[i] == current_; }
    void touch(Index i);
    std::vector<double> g_;
    std::vector<Index> parent_;
    std::vector<uint32_t> stamp_;
    std::vector<uint8_t> closed_;
    std::vector<Entry> heap_;
    std::vector<Step> steps_;
    uint32_t current_ = 0;
};

PathResult findPath(const NavSpace& space, Index start, Index goal, const AgentProfile& profile,
                    const SearchBudget& budget, SearchContext& context, bool allowPartial = false);

// ---- graph ---------------------------------------------------------------------------------------------------------

using NodeId = uint64_t;
using EdgeId = uint64_t;

struct NavNode {
    NodeId id = 0;
    DVec3 position;
    uint8_t area = 0;          // < 64
    double maxRadius = 1000;   // largest agent radius that fits, [0, 1000]
    bool blocked = false;
};

struct NavEdge {
    EdgeId id = 0;
    NodeId from = 0, to = 0;
    double cost = 1;           // finite, >= heuristicScale * |to - from|
    uint8_t area = 0;
    double maxRadius = 1000;
    bool blocked = false;
};

struct GraphSettings {
    uint32_t maxNodes = 1u << 20;  // [1, 2^26]
    uint32_t maxEdges = 1u << 22;  // [1, 2^28]
    // Lower bound of edge cost per unit of Euclidean distance; the heuristic is heuristicScale * distance.
    // 0 makes A* behave as Dijkstra (always valid). Edges cheaper than the bound are refused.
    double heuristicScale = 0;     // [0, 1e6]
    double maxCoordinate = 1e12;
};

class NavGraph : public NavSpace {
public:
    explicit NavGraph(GraphSettings settings = {});
    const GraphSettings& settings() const { return settings_; }

    // All mutations validate first and throw std::invalid_argument / std::length_error without changing anything.
    NodeId addNode(const DVec3& position, uint8_t area = 0, double maxRadius = 1000);
    bool removeNode(NodeId id);  // also removes every incident edge
    EdgeId addEdge(NodeId from, NodeId to, double cost, uint8_t area = 0, double maxRadius = 1000);
    bool removeEdge(EdgeId id);
    void setNodeBlocked(NodeId id, bool blocked);
    void setEdgeBlocked(EdgeId id, bool blocked);
    void setEdgeCost(EdgeId id, double cost);

    const NavNode* node(NodeId id) const;
    const NavEdge* edge(EdgeId id) const;
    size_t nodeCount() const { return indexOf_.size(); }
    size_t edgeCount() const { return edges_.size(); }
    std::optional<Index> index(NodeId id) const;
    NodeId idAt(Index i) const;  // 0 when invalid
    std::vector<NodeId> ids(const std::vector<Index>& path) const;

    size_t capacity() const override { return slots_.size(); }
    bool valid(Index i) const override { return i < slots_.size() && slots_[i].live; }
    uint64_t key(Index i) const override { return valid(i) ? slots_[i].node.id : 0; }
    uint64_t revision() const override { return revision_; }
    bool passable(Index i, const AgentProfile& p) const override;
    double heuristic(Index from, Index goal, const AgentProfile& p) const override;
    void expand(Index i, const AgentProfile& p, std::vector<Step>& out) const override;

    // {"format":"titan.navgraph","version":1,"settings":{...},"nextNodeId","nextEdgeId","nodes":[...],"edges":[...]}
    Json toJson() const;
    static NavGraph fromJson(const Json& j);

private:
    struct Slot {
        NavNode node;
        bool live = false;
        std::vector<EdgeId> out, in;  // ascending ids
    };
    void checkEdgeCost(const NavNode& a, const NavNode& b, double cost) const;
    NavNode* findNode(NodeId id);

    GraphSettings settings_;
    std::vector<Slot> slots_;
    std::vector<Index> freeSlots_;
    std::unordered_map<NodeId, Index> indexOf_;
    std::map<EdgeId, NavEdge> edges_;
    NodeId nextNode_ = 1;
    EdgeId nextEdge_ = 1;
    uint64_t revision_ = 1;
};

// ---- grid ----------------------------------------------------------------------------------------------------------

struct CellInfo {
    float cost = 1;  // [1, 1e4] traversal multiplier
    uint8_t area = 0;
    bool blocked = false;
    bool operator==(const CellInfo& o) const { return cost == o.cost && area == o.area && blocked == o.blocked; }
};

struct GridSettings {
    size_t maxCells = size_t(1) << 22;  // [1, 2^26]
};

// A 2D grid on the world XZ plane. Cell (x, y) covers [ox + x*s, ox + (x+1)*s) x [oz + y*s, oz + (y+1)*s).
// Agents stand at cell centres; an agent of radius R needs every cell within Chebyshev distance
// requiredClearance(R) to be unblocked and inside the grid (conservative square footprint).
class NavGrid : public NavSpace {
public:
    NavGrid(int width, int height, double cellSize, double originX = 0, double originZ = 0, GridSettings settings = {});

    int width() const { return width_; }
    int height() const { return height_; }
    double cellSize() const { return cellSize_; }
    const CellInfo& cell(int x, int y) const;  // std::out_of_range
    void setCell(int x, int y, const CellInfo& info);
    void fillRect(int x0, int y0, int x1, int y1, const CellInfo& info);  // inclusive, validated as a whole

    Index index(int x, int y) const;  // kNoIndex when outside
    void coords(Index i, int& x, int& y) const;
    std::optional<Index> cellAt(double worldX, double worldZ) const;
    DVec3 centre(Index i) const;  // y = 0
    int requiredClearance(double radius) const;
    bool clearSquare(int x, int y, int r) const;  // all cells within Chebyshev r inside the grid and unblocked

    size_t capacity() const override { return cells_.size(); }
    bool valid(Index i) const override { return i < cells_.size(); }
    uint64_t key(Index i) const override { return uint64_t(i) + 1; }
    uint64_t revision() const override { return revision_; }
    bool passable(Index i, const AgentProfile& p) const override;
    double heuristic(Index from, Index goal, const AgentProfile& p) const override;
    void expand(Index i, const AgentProfile& p, std::vector<Step>& out) const override;

    // Waypoints (cell centres) with redundant corners removed where the straight segment is safe: every cell it
    // touches is passable for the agent and no more expensive than the costliest cell of the replaced sub-path.
    // Work is bounded by maxCellChecks; past it the remaining path is kept unsmoothed.
    std::vector<DVec3> smooth(const std::vector<Index>& path, const AgentProfile& p, size_t maxCellChecks = 1u << 20) const;

    // {"format":"titan.navgrid","version":1,"width","height","cellSize","origin":[x,z],"cells":[[count,cost,area,blocked]...]}
    Json toJson() const;
    static NavGrid fromJson(const Json& j);

private:
    void rebuildBlockedSums();
    uint32_t blockedIn(int x0, int y0, int x1, int y1) const;  // inclusive, inside the grid
    bool segmentSafe(Index a, Index b, double maxCost, const AgentProfile& p, size_t& budget) const;

    int width_, height_;
    double cellSize_, originX_, originZ_;
    GridSettings settings_;
    std::vector<CellInfo> cells_;
    std::vector<uint32_t> sums_;  // (w+1) x (h+1) prefix sums of blocked cells
    uint64_t revision_ = 1;
};

// ---- multi-agent planner -------------------------------------------------------------------------------------------

using AgentId = uint64_t;

struct PlannerSettings {
    size_t maxAgents = 4096;           // [1, 2^20]
    uint32_t maxRequestsPerUpdate = 16;  // [1, 2^20]
    SearchBudget budget;
    bool allowPartial = true;
};

enum class AgentPathState { Idle, Pending, Ready };

struct AgentState {
    AgentProfile profile;
    AgentPathState state = AgentPathState::Idle;
    Index start = kNoIndex, goal = kNoIndex;
    uint64_t startKey = 0, goalKey = 0;
    PathResult result;
    uint64_t revision = 0;  // space revision the result was computed at
    uint32_t replans = 0;   // automatic re-plans after the space changed
};

// Plans paths for many agents over one space, a bounded number of searches per update(), first come first served.
// The space must outlive the planner.
class NavPlanner {
public:
    NavPlanner(const NavSpace& space, PlannerSettings settings = {});

    AgentId addAgent(const AgentProfile& profile);  // std::invalid_argument / std::length_error
    bool removeAgent(AgentId id);
    // Queues (or re-targets) a request; std::invalid_argument for an unknown agent. Start / goal validity is
    // reported in the result, not thrown.
    void request(AgentId id, Index start, Index goal);
    // Re-queues Ready agents whose path predates the space's revision (ascending agent id), then runs up to
    // maxRequestsPerUpdate searches in queue order. Returns the number of searches run.
    size_t update();

    const AgentState* agent(AgentId id) const;
    size_t pending() const { return queue_.size(); }

private:
    const NavSpace& space_;
    PlannerSettings settings_;
    std::map<AgentId, AgentState> agents_;
    std::deque<AgentId> queue_;
    AgentId nextId_ = 1;
    SearchContext context_;
};

}  // namespace titan::nav
