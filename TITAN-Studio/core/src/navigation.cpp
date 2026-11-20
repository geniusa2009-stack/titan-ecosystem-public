// TITAN Engine — navigation core. Original TITAN code.
#include "titan/navigation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace titan::nav {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kSqrt2 = 1.4142135623730951;
constexpr uint64_t kMaxId = (1ull << 53) - 1;

void fail(const std::string& msg) { throw std::invalid_argument(msg); }
bool inRange(double v, double lo, double hi) { return v >= lo && v <= hi; }  // false for NaN
bool finite(const DVec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
double distance(const DVec3& a, const DVec3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
Json vec(const DVec3& v) { return Json(Json::Array{Json(v.x), Json(v.y), Json(v.z)}); }
DVec3 vecFrom(const Json& j) {
    if (!j.isArray() || j.size() != 3) throw JsonError("expected [x, y, z]");
    return {j.at(0).asNumber(), j.at(1).asNumber(), j.at(2).asNumber()};
}
void checkVersion(const Json& j, const char* format) {
    if (!j.isObject() || j.getString("format") != format) throw JsonError(std::string("not a ") + format + " document");
    const Json* v = j.find("version");
    if (!v || !v->isNumber() || v->asNumber() != 1) throw JsonError(std::string("unsupported ") + format + " version");
}
std::string hex64(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}
uint64_t parseHex64(const std::string& s) {
    if (s.empty() || s.size() > 16) throw JsonError("expected a hexadecimal mask of 1-16 digits");
    uint64_t v = 0;
    for (char c : s) {
        int d = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1));
        if (d < 0) throw JsonError("invalid hexadecimal digit in mask");
        v = (v << 4) | uint64_t(d);
    }
    return v;
}
bool areaAllowed(const AgentProfile& p, uint8_t area) { return area < kAreaCount && ((p.allowedAreas >> area) & 1u); }

}  // namespace

// ---- profile / status ----------------------------------------------------------------------------------------------

void AgentProfile::validate() const {
    if (!inRange(radius, 0, 1000)) fail("agent radius must be finite and in [0, 1000]");
    for (int a = 0; a < kAreaCount; ++a)
        if (!inRange(areaCost[size_t(a)], 1, 1e6)) fail("area cost multiplier " + std::to_string(a) + " must be in [1, 1e6]");
}

Json AgentProfile::toJson() const {
    Json j = Json::object();
    j["radius"] = radius;
    j["allowedAreas"] = hex64(allowedAreas);
    Json costs = Json::array();
    for (double c : areaCost) costs.push_back(c);
    j["areaCost"] = std::move(costs);
    j["allowDiagonal"] = allowDiagonal;
    return j;
}

AgentProfile AgentProfile::fromJson(const Json& j) {
    if (!j.isObject()) throw JsonError("agent profile must be an object");
    AgentProfile p;
    p.radius = j["radius"].asNumber();
    p.allowedAreas = parseHex64(j["allowedAreas"].asString());
    const Json& costs = j["areaCost"];
    if (!costs.isArray() || costs.size() != size_t(kAreaCount)) throw JsonError("areaCost must hold 64 numbers");
    for (size_t a = 0; a < size_t(kAreaCount); ++a) p.areaCost[a] = costs.at(a).asNumber();
    p.allowDiagonal = j.getBool("allowDiagonal", true);
    p.validate();
    return p;
}

const char* toString(PathStatus s) {
    switch (s) {
        case PathStatus::Found: return "found";
        case PathStatus::Partial: return "partial";
        case PathStatus::NoPath: return "no-path";
        case PathStatus::BudgetExceeded: return "budget-exceeded";
        case PathStatus::InvalidStart: return "invalid-start";
        case PathStatus::InvalidGoal: return "invalid-goal";
        case PathStatus::InvalidRequest: return "invalid-request";
    }
    return "invalid-request";
}

// ---- search --------------------------------------------------------------------------------------------------------

void SearchContext::prepare(size_t n) {
    if (stamp_.size() < n) {
        g_.resize(n);
        parent_.resize(n);
        stamp_.resize(n, 0);
        closed_.resize(n);
    }
    if (++current_ == 0) {  // stamp wrapped: forget everything once
        std::fill(stamp_.begin(), stamp_.end(), 0);
        current_ = 1;
    }
    heap_.clear();
}

void SearchContext::touch(Index i) {
    stamp_[i] = current_;
    g_[i] = kInf;
    parent_[i] = kNoIndex;
    closed_[i] = 0;
}

PathResult findPath(const NavSpace& space, Index start, Index goal, const AgentProfile& profile,
                    const SearchBudget& budget, SearchContext& ctx, bool allowPartial) {
    PathResult r;
    auto finish = [&](PathStatus s, std::string why) {
        r.status = s;
        r.reason = std::move(why);
        if (!r.ok()) {
            r.path.clear();
            r.cost = 0;
        }
        return r;
    };
    try {
        profile.validate();
    } catch (const std::exception& e) {
        return finish(PathStatus::InvalidRequest, e.what());
    }
    const uint32_t kMaxBudget = 1u << 26;
    if (budget.maxExpansions < 1 || budget.maxExpansions > kMaxBudget || budget.maxPathNodes < 1 ||
        budget.maxPathNodes > kMaxBudget)
        return finish(PathStatus::InvalidRequest, "search budget out of range");
    if (space.capacity() >= size_t(kNoIndex)) return finish(PathStatus::InvalidRequest, "space is too large");
    if (!space.valid(start)) return finish(PathStatus::InvalidStart, "start does not exist");
    if (!space.passable(start, profile)) return finish(PathStatus::InvalidStart, "start is not passable for this agent");
    if (!space.valid(goal)) return finish(PathStatus::InvalidGoal, "goal does not exist");
    if (!space.passable(goal, profile)) return finish(PathStatus::InvalidGoal, "goal is not passable for this agent");
    if (start == goal) {
        r.path = {start};
        return finish(PathStatus::Found, "start is the goal");
    }

    ctx.prepare(space.capacity());
    auto worse = [](const SearchContext::Entry& a, const SearchContext::Entry& b) {  // min-heap on (f, h, index)
        if (a.f != b.f) return a.f > b.f;
        if (a.h != b.h) return a.h > b.h;
        return a.i > b.i;
    };
    auto reconstruct = [&](Index end) -> bool {
        r.path.clear();
        for (Index i = end; i != kNoIndex; i = ctx.parent_[i]) {
            if (r.path.size() >= budget.maxPathNodes) return false;
            r.path.push_back(i);
        }
        std::reverse(r.path.begin(), r.path.end());
        r.cost = ctx.g_[end];
        return true;
    };
    const double h0 = space.heuristic(start, goal, profile);
    if (!inRange(h0, 0, kInf) || !std::isfinite(h0)) return finish(PathStatus::InvalidRequest, "space heuristic is invalid");
    ctx.touch(start);
    ctx.g_[start] = 0;
    ctx.heap_.push_back({h0, h0, 0, start});

    Index best = start;
    double bestH = h0, bestG = 0;
    bool budgetHit = false;
    while (!ctx.heap_.empty()) {
        std::pop_heap(ctx.heap_.begin(), ctx.heap_.end(), worse);
        const SearchContext::Entry e = ctx.heap_.back();
        ctx.heap_.pop_back();
        if (ctx.closed_[e.i] || e.g != ctx.g_[e.i]) continue;  // stale entry
        if (e.i == goal) {
            if (!reconstruct(goal)) return finish(PathStatus::BudgetExceeded, "path is longer than maxPathNodes");
            return finish(PathStatus::Found, "found");
        }
        if (r.expansions >= budget.maxExpansions) {
            budgetHit = true;
            break;
        }
        ++r.expansions;
        ctx.closed_[e.i] = 1;
        if (e.h < bestH || (e.h == bestH && (e.g < bestG || (e.g == bestG && e.i < best)))) {
            best = e.i;
            bestH = e.h;
            bestG = e.g;
        }
        space.expand(e.i, profile, ctx.steps_);
        for (const Step& s : ctx.steps_) {
            if (!space.valid(s.to) || !inRange(s.cost, 0, kInf) || !std::isfinite(s.cost))
                return finish(PathStatus::InvalidRequest, "space produced an invalid step");
            if (ctx.seen(s.to) && ctx.closed_[s.to]) continue;
            const double ng = e.g + s.cost;
            if (!std::isfinite(ng)) return finish(PathStatus::InvalidRequest, "path cost overflowed");
            if (!ctx.seen(s.to)) ctx.touch(s.to);
            if (ng < ctx.g_[s.to]) {
                ctx.g_[s.to] = ng;
                ctx.parent_[s.to] = e.i;
                const double h = space.heuristic(s.to, goal, profile);
                if (!std::isfinite(h) || h < 0) return finish(PathStatus::InvalidRequest, "space heuristic is invalid");
                ctx.heap_.push_back({ng + h, h, ng, s.to});
                std::push_heap(ctx.heap_.begin(), ctx.heap_.end(), worse);
            } else if (ng == ctx.g_[s.to] && e.i < ctx.parent_[s.to]) {
                ctx.parent_[s.to] = e.i;  // equal cost: the lower parent index wins (order independent)
            }
        }
    }
    if (allowPartial) {
        if (!reconstruct(best)) return finish(PathStatus::BudgetExceeded, "partial path is longer than maxPathNodes");
        return finish(PathStatus::Partial, budgetHit ? "expansion budget exhausted; closest reachable node returned"
                                                     : "goal unreachable; closest reachable node returned");
    }
    return budgetHit ? finish(PathStatus::BudgetExceeded, "expansion budget exhausted")
                     : finish(PathStatus::NoPath, "goal unreachable");
}

// ---- graph ---------------------------------------------------------------------------------------------------------

NavGraph::NavGraph(GraphSettings s) : settings_(s) {
    if (s.maxNodes < 1 || s.maxNodes > (1u << 26)) fail("maxNodes must be in [1, 2^26]");
    if (s.maxEdges < 1 || s.maxEdges > (1u << 28)) fail("maxEdges must be in [1, 2^28]");
    if (!inRange(s.heuristicScale, 0, 1e6)) fail("heuristicScale must be in [0, 1e6]");
    if (!inRange(s.maxCoordinate, 1, 1e15)) fail("maxCoordinate must be in [1, 1e15]");
}

NavNode* NavGraph::findNode(NodeId id) {
    auto it = indexOf_.find(id);
    return it == indexOf_.end() ? nullptr : &slots_[it->second].node;
}
const NavNode* NavGraph::node(NodeId id) const { return const_cast<NavGraph*>(this)->findNode(id); }
const NavEdge* NavGraph::edge(EdgeId id) const {
    auto it = edges_.find(id);
    return it == edges_.end() ? nullptr : &it->second;
}
std::optional<Index> NavGraph::index(NodeId id) const {
    auto it = indexOf_.find(id);
    if (it == indexOf_.end()) return std::nullopt;
    return it->second;
}
NodeId NavGraph::idAt(Index i) const { return valid(i) ? slots_[i].node.id : 0; }
std::vector<NodeId> NavGraph::ids(const std::vector<Index>& path) const {
    std::vector<NodeId> out;
    out.reserve(path.size());
    for (Index i : path) out.push_back(idAt(i));
    return out;
}

void NavGraph::checkEdgeCost(const NavNode& a, const NavNode& b, double cost) const {
    if (!inRange(cost, 0, 1e12)) fail("edge cost must be finite and in [0, 1e12]");
    const double bound = settings_.heuristicScale * distance(a.position, b.position);
    if (cost < bound * (1 - 1e-12))
        fail("edge cost " + std::to_string(cost) + " is below heuristicScale * distance (" + std::to_string(bound) +
             "); the heuristic would overestimate");
}

NodeId NavGraph::addNode(const DVec3& p, uint8_t area, double maxRadius) {
    const double e = settings_.maxCoordinate;
    if (!finite(p) || !inRange(p.x, -e, e) || !inRange(p.y, -e, e) || !inRange(p.z, -e, e))
        fail("node position must be finite and within maxCoordinate");
    if (area >= kAreaCount) fail("node area must be < 64");
    if (!inRange(maxRadius, 0, 1000)) fail("node maxRadius must be in [0, 1000]");
    if (indexOf_.size() >= settings_.maxNodes) throw std::length_error("navigation graph node limit reached");
    if (nextNode_ > kMaxId) throw std::length_error("navigation node ids exhausted");
    const bool reuse = !freeSlots_.empty();
    const Index slot = reuse ? freeSlots_.back() : Index(slots_.size());
    if (!reuse) slots_.emplace_back();  // may throw before anything else changed
    try {
        indexOf_.emplace(nextNode_, slot);
    } catch (...) {
        if (!reuse) slots_.pop_back();
        throw;
    }
    if (reuse) freeSlots_.pop_back();
    Slot& s = slots_[slot];
    s.node = NavNode{nextNode_, p, area, maxRadius, false};
    s.live = true;
    s.out.clear();
    s.in.clear();
    ++revision_;
    return nextNode_++;
}

bool NavGraph::removeNode(NodeId id) {
    auto it = indexOf_.find(id);
    if (it == indexOf_.end()) return false;
    Slot& s = slots_[it->second];
    std::vector<EdgeId> incident = s.out;
    incident.insert(incident.end(), s.in.begin(), s.in.end());
    for (EdgeId e : incident) removeEdge(e);
    s.live = false;
    s.out.clear();
    s.in.clear();
    freeSlots_.push_back(it->second);
    indexOf_.erase(it);
    ++revision_;
    return true;
}

EdgeId NavGraph::addEdge(NodeId from, NodeId to, double cost, uint8_t area, double maxRadius) {
    auto a = indexOf_.find(from), b = indexOf_.find(to);
    if (a == indexOf_.end() || b == indexOf_.end()) fail("edge references a missing node");
    if (from == to) fail("self-loop edges are not allowed");
    checkEdgeCost(slots_[a->second].node, slots_[b->second].node, cost);
    if (area >= kAreaCount) fail("edge area must be < 64");
    if (!inRange(maxRadius, 0, 1000)) fail("edge maxRadius must be in [0, 1000]");
    if (edges_.size() >= settings_.maxEdges) throw std::length_error("navigation graph edge limit reached");
    if (nextEdge_ > kMaxId) throw std::length_error("navigation edge ids exhausted");
    Slot& sa = slots_[a->second];
    Slot& sb = slots_[b->second];
    sa.out.reserve(sa.out.size() + 1);  // reserve first: nothing below can throw
    sb.in.reserve(sb.in.size() + 1);
    const EdgeId id = nextEdge_;
    edges_.emplace(id, NavEdge{id, from, to, cost, area, maxRadius, false});
    sa.out.push_back(id);  // ids only grow: lists stay sorted
    sb.in.push_back(id);
    ++nextEdge_;
    ++revision_;
    return id;
}

bool NavGraph::removeEdge(EdgeId id) {
    auto it = edges_.find(id);
    if (it == edges_.end()) return false;
    auto drop = [id](std::vector<EdgeId>& v) { v.erase(std::remove(v.begin(), v.end(), id), v.end()); };
    drop(slots_[indexOf_.at(it->second.from)].out);
    drop(slots_[indexOf_.at(it->second.to)].in);
    edges_.erase(it);
    ++revision_;
    return true;
}

void NavGraph::setNodeBlocked(NodeId id, bool blocked) {
    NavNode* n = findNode(id);
    if (!n) fail("unknown node " + std::to_string(id));
    n->blocked = blocked;
    ++revision_;
}

void NavGraph::setEdgeBlocked(EdgeId id, bool blocked) {
    auto it = edges_.find(id);
    if (it == edges_.end()) fail("unknown edge " + std::to_string(id));
    it->second.blocked = blocked;
    ++revision_;
}

void NavGraph::setEdgeCost(EdgeId id, double cost) {
    auto it = edges_.find(id);
    if (it == edges_.end()) fail("unknown edge " + std::to_string(id));
    checkEdgeCost(*node(it->second.from), *node(it->second.to), cost);
    it->second.cost = cost;
    ++revision_;
}

bool NavGraph::passable(Index i, const AgentProfile& p) const {
    if (!valid(i)) return false;
    const NavNode& n = slots_[i].node;
    return !n.blocked && areaAllowed(p, n.area) && p.radius <= n.maxRadius;
}

double NavGraph::heuristic(Index from, Index goal, const AgentProfile&) const {
    if (settings_.heuristicScale == 0 || !valid(from) || !valid(goal)) return 0;
    return settings_.heuristicScale * distance(slots_[from].node.position, slots_[goal].node.position);
}

void NavGraph::expand(Index i, const AgentProfile& p, std::vector<Step>& out) const {
    out.clear();
    if (!valid(i)) return;
    for (EdgeId id : slots_[i].out) {
        const NavEdge& e = edges_.at(id);
        if (e.blocked || !areaAllowed(p, e.area) || p.radius > e.maxRadius) continue;
        const Index to = indexOf_.at(e.to);
        if (!passable(to, p)) continue;
        out.push_back({to, e.cost * p.areaCost[e.area]});
    }
}

Json NavGraph::toJson() const {
    Json j = Json::object();
    j["format"] = "titan.navgraph";
    j["version"] = 1;
    Json s = Json::object();
    s["maxNodes"] = settings_.maxNodes;
    s["maxEdges"] = settings_.maxEdges;
    s["heuristicScale"] = settings_.heuristicScale;
    s["maxCoordinate"] = settings_.maxCoordinate;
    j["settings"] = std::move(s);
    j["nextNodeId"] = nextNode_;
    j["nextEdgeId"] = nextEdge_;
    std::vector<NodeId> nodeIds;
    for (const auto& [id, slot] : indexOf_) nodeIds.push_back(id);
    std::sort(nodeIds.begin(), nodeIds.end());
    Json ns = Json::array();
    for (NodeId id : nodeIds) {
        const NavNode& n = *node(id);
        Json o = Json::object();
        o["id"] = n.id;
        o["position"] = vec(n.position);
        o["area"] = n.area;
        o["maxRadius"] = n.maxRadius;
        o["blocked"] = n.blocked;
        ns.push_back(std::move(o));
    }
    j["nodes"] = std::move(ns);
    Json es = Json::array();
    for (const auto& [id, e] : edges_) {
        Json o = Json::object();
        o["id"] = e.id;
        o["from"] = e.from;
        o["to"] = e.to;
        o["cost"] = e.cost;
        o["area"] = e.area;
        o["maxRadius"] = e.maxRadius;
        o["blocked"] = e.blocked;
        es.push_back(std::move(o));
    }
    j["edges"] = std::move(es);
    return j;
}

NavGraph NavGraph::fromJson(const Json& j) {
    checkVersion(j, "titan.navgraph");
    const Json& s = j["settings"];
    if (!s.isObject()) throw JsonError("titan.navgraph needs 'settings'");
    GraphSettings gs;
    gs.maxNodes = uint32_t(std::min<uint64_t>(s["maxNodes"].asUInt(), 0xFFFFFFFFu));
    gs.maxEdges = uint32_t(std::min<uint64_t>(s["maxEdges"].asUInt(), 0xFFFFFFFFu));
    gs.heuristicScale = s["heuristicScale"].asNumber();
    gs.maxCoordinate = s["maxCoordinate"].asNumber();
    NavGraph g(gs);
    const Json& ns = j["nodes"];
    const Json& es = j["edges"];
    if (!ns.isArray() || !es.isArray()) throw JsonError("titan.navgraph needs 'nodes' and 'edges' arrays");
    if (ns.size() > gs.maxNodes || es.size() > gs.maxEdges) fail("titan.navgraph exceeds its own limits");
    for (const Json& o : ns.asArray()) {
        if (!o.isObject()) throw JsonError("node must be an object");
        const uint64_t id = o["id"].asUInt();
        if (id < g.nextNode_ || id > kMaxId) fail("node ids must be positive and ascending");
        const uint64_t area = o["area"].asUInt();
        if (area >= uint64_t(kAreaCount)) fail("node area must be < 64");
        g.nextNode_ = id;
        g.addNode(vecFrom(o["position"]), uint8_t(area), o["maxRadius"].asNumber());
        if (o["blocked"].asBool()) g.findNode(id)->blocked = true;
    }
    for (const Json& o : es.asArray()) {
        if (!o.isObject()) throw JsonError("edge must be an object");
        const uint64_t id = o["id"].asUInt();
        if (id < g.nextEdge_ || id > kMaxId) fail("edge ids must be positive and ascending");
        const uint64_t area = o["area"].asUInt();
        if (area >= uint64_t(kAreaCount)) fail("edge area must be < 64");
        g.nextEdge_ = id;
        g.addEdge(o["from"].asUInt(), o["to"].asUInt(), o["cost"].asNumber(), uint8_t(area), o["maxRadius"].asNumber());
        if (o["blocked"].asBool()) g.edges_.at(id).blocked = true;
    }
    const uint64_t nn = j["nextNodeId"].asUInt(), ne = j["nextEdgeId"].asUInt();
    if (nn < g.nextNode_ || ne < g.nextEdge_ || nn > kMaxId + 1 || ne > kMaxId + 1) fail("next ids must exceed every id");
    g.nextNode_ = nn;
    g.nextEdge_ = ne;
    g.revision_ = 1;
    return g;
}

// ---- grid ----------------------------------------------------------------------------------------------------------

namespace {
void checkCell(const CellInfo& c) {
    if (!inRange(c.cost, 1, 1e4)) fail("cell cost must be finite and in [1, 1e4]");
    if (c.area >= kAreaCount) fail("cell area must be < 64");
}
}  // namespace

NavGrid::NavGrid(int width, int height, double cellSize, double originX, double originZ, GridSettings settings)
    : width_(width), height_(height), cellSize_(cellSize), originX_(originX), originZ_(originZ), settings_(settings) {
    if (settings_.maxCells < 1 || settings_.maxCells > (size_t(1) << 26)) fail("maxCells must be in [1, 2^26]");
    if (width < 1 || height < 1 || uint64_t(width) * uint64_t(height) > settings_.maxCells)
        fail("grid dimensions must be positive and within maxCells");
    if (!inRange(cellSize, 1e-6, 1e4)) fail("cellSize must be in [1e-6, 1e4]");
    if (!inRange(originX, -1e12, 1e12) || !inRange(originZ, -1e12, 1e12)) fail("grid origin must be finite");
    cells_.assign(size_t(width) * size_t(height), CellInfo{});
    sums_.assign(size_t(width + 1) * size_t(height + 1), 0);
}

const CellInfo& NavGrid::cell(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) throw std::out_of_range("grid cell out of range");
    return cells_[size_t(y) * size_t(width_) + size_t(x)];
}

void NavGrid::setCell(int x, int y, const CellInfo& info) { fillRect(x, y, x, y, info); }

void NavGrid::fillRect(int x0, int y0, int x1, int y1, const CellInfo& info) {
    if (x0 > x1 || y0 > y1 || x0 < 0 || y0 < 0 || x1 >= width_ || y1 >= height_) fail("grid rectangle out of range");
    checkCell(info);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) cells_[size_t(y) * size_t(width_) + size_t(x)] = info;
    rebuildBlockedSums();
    ++revision_;
}

void NavGrid::rebuildBlockedSums() {
    const size_t W = size_t(width_) + 1;
    for (int y = 0; y < height_; ++y) {
        uint32_t row = 0;
        for (int x = 0; x < width_; ++x) {
            row += cells_[size_t(y) * size_t(width_) + size_t(x)].blocked ? 1u : 0u;
            sums_[(size_t(y) + 1) * W + size_t(x) + 1] = sums_[size_t(y) * W + size_t(x) + 1] + row;
        }
    }
}

uint32_t NavGrid::blockedIn(int x0, int y0, int x1, int y1) const {
    const size_t W = size_t(width_) + 1;
    // Unsigned wrap-around is well defined and the true result is non-negative, so the sum is exact.
    return sums_[(size_t(y1) + 1) * W + size_t(x1) + 1] - sums_[size_t(y0) * W + size_t(x1) + 1] -
           sums_[(size_t(y1) + 1) * W + size_t(x0)] + sums_[size_t(y0) * W + size_t(x0)];
}

Index NavGrid::index(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return kNoIndex;
    return Index(size_t(y) * size_t(width_) + size_t(x));
}

void NavGrid::coords(Index i, int& x, int& y) const {
    x = int(i % uint32_t(width_));
    y = int(i / uint32_t(width_));
}

std::optional<Index> NavGrid::cellAt(double wx, double wz) const {
    if (!std::isfinite(wx) || !std::isfinite(wz)) return std::nullopt;
    const double fx = std::floor((wx - originX_) / cellSize_), fz = std::floor((wz - originZ_) / cellSize_);
    if (!(fx >= 0 && fx < width_ && fz >= 0 && fz < height_)) return std::nullopt;
    return index(int(fx), int(fz));
}

DVec3 NavGrid::centre(Index i) const {
    int x, y;
    coords(i, x, y);
    return {originX_ + (x + 0.5) * cellSize_, 0, originZ_ + (y + 0.5) * cellSize_};
}

int NavGrid::requiredClearance(double radius) const {
    if (!(radius > 0)) return 0;
    const double k = std::ceil(radius / cellSize_ + 0.5) - 1;
    const double cap = double(std::max(width_, height_));
    return int(std::clamp(k, 0.0, cap));
}

bool NavGrid::clearSquare(int x, int y, int r) const {
    const int64_t x0 = int64_t(x) - r, y0 = int64_t(y) - r, x1 = int64_t(x) + r, y1 = int64_t(y) + r;
    if (r < 0 || x0 < 0 || y0 < 0 || x1 >= width_ || y1 >= height_) return false;
    return blockedIn(int(x0), int(y0), int(x1), int(y1)) == 0;
}

bool NavGrid::passable(Index i, const AgentProfile& p) const {
    if (!valid(i)) return false;
    int x, y;
    coords(i, x, y);
    return areaAllowed(p, cells_[i].area) && clearSquare(x, y, requiredClearance(p.radius));
}

double NavGrid::heuristic(Index from, Index goal, const AgentProfile& p) const {
    int ax, ay, bx, by;
    coords(from, ax, ay);
    coords(goal, bx, by);
    const double dx = std::abs(ax - bx), dy = std::abs(ay - by);
    if (!p.allowDiagonal) return cellSize_ * (dx + dy);
    return cellSize_ * (std::max(dx, dy) - std::min(dx, dy) + kSqrt2 * std::min(dx, dy));
}

void NavGrid::expand(Index i, const AgentProfile& p, std::vector<Step>& out) const {
    out.clear();
    if (!valid(i)) return;
    int x, y;
    coords(i, x, y);
    const int r = requiredClearance(p.radius);
    auto open = [&](int cx, int cy) {
        return cx >= 0 && cy >= 0 && cx < width_ && cy < height_ &&
               areaAllowed(p, cells_[size_t(cy) * size_t(width_) + size_t(cx)].area) && clearSquare(cx, cy, r);
    };
    auto stepCost = [&](int cx, int cy, double len) {
        const CellInfo& c = cells_[size_t(cy) * size_t(width_) + size_t(cx)];
        return cellSize_ * len * double(c.cost) * p.areaCost[c.area];
    };
    static const int ortho[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
    for (const auto& d : ortho)
        if (open(x + d[0], y + d[1])) out.push_back({index(x + d[0], y + d[1]), stepCost(x + d[0], y + d[1], 1)});
    if (!p.allowDiagonal) return;
    static const int diag[4][2] = {{1, -1}, {1, 1}, {-1, 1}, {-1, -1}};
    for (const auto& d : diag)  // no corner cutting: both orthogonal neighbours must be open too
        if (open(x + d[0], y + d[1]) && open(x + d[0], y) && open(x, y + d[1]))
            out.push_back({index(x + d[0], y + d[1]), stepCost(x + d[0], y + d[1], kSqrt2)});
}

bool NavGrid::segmentSafe(Index a, Index b, double maxCost, const AgentProfile& p, size_t& budget) const {
    int ax, ay, bx, by;
    coords(a, ax, ay);
    coords(b, bx, by);
    const int r = requiredClearance(p.radius);
    auto ok = [&](int x, int y) {
        if (budget == 0) return false;
        --budget;
        const CellInfo& c = cells_[size_t(y) * size_t(width_) + size_t(x)];
        return areaAllowed(p, c.area) && clearSquare(x, y, r) && double(c.cost) * p.areaCost[c.area] <= maxCost;
    };
    // Exact traversal between cell centres in integer arithmetic: the next x boundary is crossed at
    // t = (2kx + 1) / (2|dx|), the next y boundary at (2ky + 1) / (2|dy|). Exact corners check both side cells.
    const int64_t dx = bx - ax, dy = by - ay, adx = std::abs(dx), ady = std::abs(dy);
    const int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
    int x = ax, y = ay;
    int64_t kx = 0, ky = 0;
    for (int64_t guard = 0; guard <= adx + ady + 1; ++guard) {
        if (!ok(x, y)) return false;
        if (x == bx && y == by) return true;
        int64_t cmp;  // < 0: x boundary first, > 0: y first, 0: corner
        if (adx == 0) cmp = 1;
        else if (ady == 0) cmp = -1;
        else cmp = (2 * kx + 1) * ady - (2 * ky + 1) * adx;
        if (cmp < 0) {
            x += sx;
            ++kx;
        } else if (cmp > 0) {
            y += sy;
            ++ky;
        } else {
            if (!ok(x + sx, y) || !ok(x, y + sy)) return false;
            x += sx;
            y += sy;
            ++kx;
            ++ky;
        }
    }
    return false;  // unreachable for valid input
}

std::vector<DVec3> NavGrid::smooth(const std::vector<Index>& path, const AgentProfile& p, size_t maxCellChecks) const {
    p.validate();
    for (Index i : path)
        if (!valid(i)) fail("path contains an index outside the grid");
    std::vector<DVec3> out;
    if (path.empty()) return out;
    out.push_back(centre(path[0]));
    if (path.size() == 1) return out;
    auto cost = [&](Index i) { return double(cells_[i].cost) * p.areaCost[cells_[i].area]; };
    size_t budget = maxCellChecks;
    size_t anchor = 0;
    double maxCost = std::max(cost(path[0]), cost(path[1]));
    for (size_t i = 2; i < path.size(); ++i) {
        maxCost = std::max(maxCost, cost(path[i]));
        if (budget == 0) {  // out of budget: keep the rest exactly as planned
            for (size_t k = anchor + 1; k + 1 < path.size(); ++k) out.push_back(centre(path[k]));
            out.push_back(centre(path.back()));
            return out;
        }
        if (!segmentSafe(path[anchor], path[i], maxCost, p, budget)) {
            anchor = i - 1;
            out.push_back(centre(path[anchor]));
            maxCost = std::max(cost(path[anchor]), cost(path[i]));
        }
    }
    out.push_back(centre(path.back()));
    return out;
}

Json NavGrid::toJson() const {
    Json j = Json::object();
    j["format"] = "titan.navgrid";
    j["version"] = 1;
    j["width"] = width_;
    j["height"] = height_;
    j["cellSize"] = cellSize_;
    j["origin"] = Json(Json::Array{Json(originX_), Json(originZ_)});
    j["maxCells"] = uint64_t(settings_.maxCells);
    Json runs = Json::array();
    for (size_t i = 0; i < cells_.size();) {
        size_t n = 1;
        while (i + n < cells_.size() && cells_[i + n] == cells_[i]) ++n;
        runs.push_back(Json(Json::Array{Json(uint64_t(n)), Json(cells_[i].cost), Json(cells_[i].area), Json(cells_[i].blocked)}));
        i += n;
    }
    j["cells"] = std::move(runs);
    return j;
}

NavGrid NavGrid::fromJson(const Json& j) {
    checkVersion(j, "titan.navgrid");
    const Json& o = j["origin"];
    if (!o.isArray() || o.size() != 2) throw JsonError("origin must be [x, z]");
    GridSettings gs;
    gs.maxCells = size_t(std::min<uint64_t>(j["maxCells"].asUInt(), uint64_t(1) << 32));
    const int64_t w = j["width"].asInt(), h = j["height"].asInt();
    if (w < 1 || h < 1 || w > (1 << 26) || h > (1 << 26)) fail("grid dimensions out of range");
    NavGrid g(int(w), int(h), j["cellSize"].asNumber(), o.at(0).asNumber(), o.at(1).asNumber(), gs);
    const Json& runs = j["cells"];
    if (!runs.isArray()) throw JsonError("titan.navgrid needs a 'cells' array");
    size_t at = 0;
    for (const Json& run : runs.asArray()) {
        if (!run.isArray() || run.size() != 4) throw JsonError("cell run must be [count, cost, area, blocked]");
        const uint64_t n = run.at(0).asUInt();
        if (n < 1 || n > g.cells_.size() - at) fail("cell runs do not match the grid size");
        CellInfo c;
        c.cost = float(run.at(1).asNumber());
        const uint64_t area = run.at(2).asUInt();
        if (area >= uint64_t(kAreaCount)) fail("cell area must be < 64");
        c.area = uint8_t(area);
        c.blocked = run.at(3).asBool();
        checkCell(c);
        std::fill(g.cells_.begin() + std::ptrdiff_t(at), g.cells_.begin() + std::ptrdiff_t(at + n), c);
        at += n;
    }
    if (at != g.cells_.size()) fail("cell runs do not cover the grid");
    g.rebuildBlockedSums();
    return g;
}

// ---- planner -------------------------------------------------------------------------------------------------------

NavPlanner::NavPlanner(const NavSpace& space, PlannerSettings s) : space_(space), settings_(s) {
    if (s.maxAgents < 1 || s.maxAgents > (size_t(1) << 20)) fail("maxAgents must be in [1, 2^20]");
    if (s.maxRequestsPerUpdate < 1 || s.maxRequestsPerUpdate > (1u << 20)) fail("maxRequestsPerUpdate must be in [1, 2^20]");
    if (s.budget.maxExpansions < 1 || s.budget.maxExpansions > (1u << 26) || s.budget.maxPathNodes < 1 ||
        s.budget.maxPathNodes > (1u << 26))
        fail("planner search budget out of range");
}

AgentId NavPlanner::addAgent(const AgentProfile& profile) {
    profile.validate();
    if (agents_.size() >= settings_.maxAgents) throw std::length_error("navigation agent limit reached");
    const AgentId id = nextId_;
    AgentState st;
    st.profile = profile;
    agents_.emplace(id, std::move(st));
    ++nextId_;
    return id;
}

bool NavPlanner::removeAgent(AgentId id) {
    if (!agents_.erase(id)) return false;
    queue_.erase(std::remove(queue_.begin(), queue_.end(), id), queue_.end());
    return true;
}

void NavPlanner::request(AgentId id, Index start, Index goal) {
    auto it = agents_.find(id);
    if (it == agents_.end()) fail("unknown navigation agent " + std::to_string(id));
    AgentState& a = it->second;
    const bool queued = a.state == AgentPathState::Pending;
    if (!queued) queue_.push_back(id);  // may throw before the state changes
    a.start = start;
    a.goal = goal;
    a.startKey = space_.valid(start) ? space_.key(start) : 0;
    a.goalKey = space_.valid(goal) ? space_.key(goal) : 0;
    a.state = AgentPathState::Pending;
}

size_t NavPlanner::update() {
    const uint64_t rev = space_.revision();
    for (auto& [id, a] : agents_) {
        if (a.state == AgentPathState::Ready && a.revision != rev) {
            queue_.push_back(id);
            a.state = AgentPathState::Pending;
            ++a.replans;
        }
    }
    size_t ran = 0;
    while (!queue_.empty() && ran < settings_.maxRequestsPerUpdate) {
        const AgentId id = queue_.front();
        queue_.pop_front();
        auto it = agents_.find(id);
        if (it == agents_.end() || it->second.state != AgentPathState::Pending) continue;
        AgentState& a = it->second;
        if (!space_.valid(a.start) || space_.key(a.start) != a.startKey || a.startKey == 0) {
            a.result = PathResult{};
            a.result.status = PathStatus::InvalidStart;
            a.result.reason = "start no longer exists (or never did)";
        } else if (!space_.valid(a.goal) || space_.key(a.goal) != a.goalKey || a.goalKey == 0) {
            a.result = PathResult{};
            a.result.status = PathStatus::InvalidGoal;
            a.result.reason = "goal no longer exists (or never did)";
        } else {
            a.result = findPath(space_, a.start, a.goal, a.profile, settings_.budget, context_, settings_.allowPartial);
        }
        a.state = AgentPathState::Ready;
        a.revision = rev;
        ++ran;
    }
    return ran;
}

const AgentState* NavPlanner::agent(AgentId id) const {
    auto it = agents_.find(id);
    return it == agents_.end() ? nullptr : &it->second;
}

}  // namespace titan::nav
