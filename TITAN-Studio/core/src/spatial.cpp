// TITAN Engine — spatial world index (dynamic AABB tree). Original TITAN code.
#include "titan/spatial.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace titan::spatial {
namespace {

void fail(const std::string& msg) { throw std::invalid_argument(msg); }
bool inRange(double v, double lo, double hi) { return v >= lo && v <= hi; }  // false for NaN
bool finite(const DVec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
double dot(const DVec3& a, const DVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Bounds unite(const Bounds& a, const Bounds& b) {
    return {{std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y), std::min(a.min.z, b.min.z)},
            {std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y), std::max(a.max.z, b.max.z)}};
}

double surfaceArea(const Bounds& b) {
    const double dx = b.max.x - b.min.x, dy = b.max.y - b.min.y, dz = b.max.z - b.min.z;
    return 2 * (dx * dy + dy * dz + dz * dx);
}

void checkQueryBox(const Bounds& b) {
    if (!finite(b.min) || !finite(b.max) || b.min.x > b.max.x || b.min.y > b.max.y || b.min.z > b.max.z)
        fail("query bounds must be finite with min <= max");
}

// Slab test against [0, maxT]; `d` is a unit direction.
bool rayHitsBox(const DVec3& o, const DVec3& d, double maxT, const Bounds& b, double& tEnter) {
    double tmin = 0, tmax = maxT;
    const double oc[3] = {o.x, o.y, o.z}, dc[3] = {d.x, d.y, d.z};
    const double lo[3] = {b.min.x, b.min.y, b.min.z}, hi[3] = {b.max.x, b.max.y, b.max.z};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(dc[i]) < 1e-300) {
            if (oc[i] < lo[i] || oc[i] > hi[i]) return false;
            continue;
        }
        const double inv = 1.0 / dc[i];
        double t1 = (lo[i] - oc[i]) * inv, t2 = (hi[i] - oc[i]) * inv;
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return false;
    }
    tEnter = tmin;
    return true;
}

double distanceSquared(const DVec3& p, const Bounds& b) {
    auto axis = [](double v, double lo, double hi) {
        const double d = v < lo ? lo - v : (v > hi ? v - hi : 0.0);
        return d * d;
    };
    return axis(p.x, b.min.x, b.max.x) + axis(p.y, b.min.y, b.max.y) + axis(p.z, b.min.z, b.max.z);
}

// Fixed inline stack with heap spill: traversal allocates nothing for any realistic tree height.
class NodeStack {
public:
    void push(int32_t i) {
        if (top_ < kInline) inline_[top_++] = i;
        else spill_.push_back(i);
    }
    int32_t pop() {
        if (!spill_.empty()) {
            const int32_t v = spill_.back();
            spill_.pop_back();
            return v;
        }
        return inline_[--top_];
    }
    bool empty() const { return top_ == 0 && spill_.empty(); }

private:
    static constexpr size_t kInline = 128;
    int32_t inline_[kInline];
    size_t top_ = 0;
    std::vector<int32_t> spill_;
};

}  // namespace

Bounds Bounds::around(const DVec3& c, const DVec3& h) { return {{c.x - h.x, c.y - h.y, c.z - h.z}, {c.x + h.x, c.y + h.y, c.z + h.z}}; }

// ---- frustum -----------------------------------------------------------------------------------------------------

Frustum Frustum::fromViewProjection(const Mat4& m, ClipDepth depth) {
    double r[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            r[i][j] = m.at(i, j);
            if (!std::isfinite(r[i][j])) fail("view-projection matrix must be finite");
        }
    auto plane = [&](int row, double sign, bool alone) {
        Plane p;
        const double a = alone ? 0.0 : 1.0;
        p.normal = {a * r[3][0] + sign * r[row][0], a * r[3][1] + sign * r[row][1], a * r[3][2] + sign * r[row][2]};
        p.d = a * r[3][3] + sign * r[row][3];
        const double len = std::sqrt(dot(p.normal, p.normal));
        if (!(len > 1e-12) || !std::isfinite(len)) fail("degenerate view-projection matrix");
        p.normal = {p.normal.x / len, p.normal.y / len, p.normal.z / len};
        p.d /= len;
        return p;
    };
    Frustum f;
    f.planes[0] = plane(0, 1, false);   // left
    f.planes[1] = plane(0, -1, false);  // right
    f.planes[2] = plane(1, 1, false);   // bottom
    f.planes[3] = plane(1, -1, false);  // top
    f.planes[4] = depth == ClipDepth::ZeroToOne ? plane(2, 1, true) : plane(2, 1, false);  // near
    f.planes[5] = plane(2, -1, false);  // far
    return f;
}

bool Frustum::outside(const Bounds& b) const {
    for (const Plane& p : planes) {
        const DVec3 v{p.normal.x >= 0 ? b.max.x : b.min.x, p.normal.y >= 0 ? b.max.y : b.min.y,
                      p.normal.z >= 0 ? b.max.z : b.min.z};
        if (dot(p.normal, v) + p.d < 0) return true;
    }
    return false;
}

// ---- index -------------------------------------------------------------------------------------------------------

SpatialIndex::SpatialIndex(Settings settings) : settings_(settings) {
    if (!inRange(settings_.margin, 0, 1e6)) fail("spatial margin must be in [0, 1e6]");
    if (!inRange(settings_.marginFraction, 0, 10)) fail("spatial marginFraction must be in [0, 10]");
    if (settings_.maxProxies < 1 || settings_.maxProxies > (size_t(1) << 30)) fail("maxProxies must be in [1, 2^30]");
    if (!(settings_.maxCoordinate > 0) || !(settings_.maxCoordinate <= 1e15)) fail("maxCoordinate must be in (0, 1e15]");
}

void SpatialIndex::checkBounds(const Bounds& b) const {
    const double e = settings_.maxCoordinate;
    for (const DVec3* v : {&b.min, &b.max})
        if (!inRange(v->x, -e, e) || !inRange(v->y, -e, e) || !inRange(v->z, -e, e))
            fail("bounds must be finite and within +/- maxCoordinate");
    if (b.min.x > b.max.x || b.min.y > b.max.y || b.min.z > b.max.z) fail("bounds must have min <= max on every axis");
}

Bounds SpatialIndex::fatten(const Bounds& b) const {
    const double m = settings_.margin, f = settings_.marginFraction;
    const DVec3 g{m + f * (b.max.x - b.min.x), m + f * (b.max.y - b.min.y), m + f * (b.max.z - b.min.z)};
    return {{b.min.x - g.x, b.min.y - g.y, b.min.z - g.z}, {b.max.x + g.x, b.max.y + g.y, b.max.z + g.z}};
}

int32_t SpatialIndex::allocate() {
    int32_t i;
    if (free_ >= 0) {
        i = free_;
        free_ = nodes_[size_t(i)].parent;
        --freeCount_;
        nodes_[size_t(i)] = Node{};
    } else {
        i = int32_t(nodes_.size());
        nodes_.emplace_back();
    }
    return i;
}

void SpatialIndex::release(int32_t i) {
    Node& n = nodes_[size_t(i)];
    n = Node{};
    n.height = -1;
    n.parent = free_;
    free_ = i;
    ++freeCount_;
}

int32_t SpatialIndex::balance(int32_t iA) {
    Node& A = nodes_[size_t(iA)];
    if (A.leaf() || A.height < 2) return iA;
    const int32_t iB = A.child1, iC = A.child2;
    Node& B = nodes_[size_t(iB)];
    Node& C = nodes_[size_t(iC)];
    const int32_t bal = C.height - B.height;

    if (bal > 1) {  // rotate C up
        const int32_t iF = C.child1, iG = C.child2;
        Node& F = nodes_[size_t(iF)];
        Node& G = nodes_[size_t(iG)];
        C.child1 = iA;
        C.parent = A.parent;
        A.parent = iC;
        if (C.parent >= 0) {
            Node& P = nodes_[size_t(C.parent)];
            (P.child1 == iA ? P.child1 : P.child2) = iC;
        } else {
            root_ = iC;
        }
        if (F.height > G.height) {
            C.child2 = iF;
            A.child2 = iG;
            G.parent = iA;
            A.box = unite(B.box, G.box);
            C.box = unite(A.box, F.box);
            A.height = 1 + std::max(B.height, G.height);
            C.height = 1 + std::max(A.height, F.height);
        } else {
            C.child2 = iG;
            A.child2 = iF;
            F.parent = iA;
            A.box = unite(B.box, F.box);
            C.box = unite(A.box, G.box);
            A.height = 1 + std::max(B.height, F.height);
            C.height = 1 + std::max(A.height, G.height);
        }
        return iC;
    }
    if (bal < -1) {  // rotate B up
        const int32_t iD = B.child1, iE = B.child2;
        Node& D = nodes_[size_t(iD)];
        Node& E = nodes_[size_t(iE)];
        B.child1 = iA;
        B.parent = A.parent;
        A.parent = iB;
        if (B.parent >= 0) {
            Node& P = nodes_[size_t(B.parent)];
            (P.child1 == iA ? P.child1 : P.child2) = iB;
        } else {
            root_ = iB;
        }
        if (D.height > E.height) {
            B.child2 = iD;
            A.child1 = iE;
            E.parent = iA;
            A.box = unite(C.box, E.box);
            B.box = unite(A.box, D.box);
            A.height = 1 + std::max(C.height, E.height);
            B.height = 1 + std::max(A.height, D.height);
        } else {
            B.child2 = iE;
            A.child1 = iD;
            D.parent = iA;
            A.box = unite(C.box, D.box);
            B.box = unite(A.box, E.box);
            A.height = 1 + std::max(C.height, D.height);
            B.height = 1 + std::max(A.height, E.height);
        }
        return iB;
    }
    return iA;
}

void SpatialIndex::insertLeaf(int32_t leaf) {
    if (root_ < 0) {
        root_ = leaf;
        nodes_[size_t(leaf)].parent = -1;
        return;
    }
    const Bounds lb = nodes_[size_t(leaf)].box;
    int32_t index = root_;
    while (!nodes_[size_t(index)].leaf()) {
        const Node& n = nodes_[size_t(index)];
        const double area = surfaceArea(n.box);
        const double combined = surfaceArea(unite(n.box, lb));
        const double cost = 2 * combined;               // new parent for this node and the leaf
        const double inherit = 2 * (combined - area);   // growth pushed onto every ancestor below here
        auto childCost = [&](int32_t c) {
            const Node& cn = nodes_[size_t(c)];
            const double grown = surfaceArea(unite(lb, cn.box));
            return (cn.leaf() ? grown : grown - surfaceArea(cn.box)) + inherit;
        };
        const double c1 = childCost(n.child1), c2 = childCost(n.child2);
        if (cost < c1 && cost < c2) break;
        index = c1 < c2 ? n.child1 : n.child2;
    }
    const int32_t sibling = index;
    const int32_t oldParent = nodes_[size_t(sibling)].parent;
    const int32_t parent = allocate();  // capacity is guaranteed by the caller: no reallocation here
    Node& P = nodes_[size_t(parent)];
    P.parent = oldParent;
    P.box = unite(lb, nodes_[size_t(sibling)].box);
    P.height = nodes_[size_t(sibling)].height + 1;
    P.child1 = sibling;
    P.child2 = leaf;
    nodes_[size_t(sibling)].parent = parent;
    nodes_[size_t(leaf)].parent = parent;
    if (oldParent >= 0) {
        Node& O = nodes_[size_t(oldParent)];
        (O.child1 == sibling ? O.child1 : O.child2) = parent;
    } else {
        root_ = parent;
    }
    for (int32_t i = parent; i >= 0;) {
        i = balance(i);
        Node& n = nodes_[size_t(i)];
        n.height = 1 + std::max(nodes_[size_t(n.child1)].height, nodes_[size_t(n.child2)].height);
        n.box = unite(nodes_[size_t(n.child1)].box, nodes_[size_t(n.child2)].box);
        i = n.parent;
    }
}

void SpatialIndex::removeLeaf(int32_t leaf) {
    if (leaf == root_) {
        root_ = -1;
        return;
    }
    const int32_t parent = nodes_[size_t(leaf)].parent;
    const int32_t grand = nodes_[size_t(parent)].parent;
    const int32_t sibling =
        nodes_[size_t(parent)].child1 == leaf ? nodes_[size_t(parent)].child2 : nodes_[size_t(parent)].child1;
    if (grand >= 0) {
        Node& G = nodes_[size_t(grand)];
        (G.child1 == parent ? G.child1 : G.child2) = sibling;
        nodes_[size_t(sibling)].parent = grand;
        release(parent);
        for (int32_t i = grand; i >= 0;) {
            i = balance(i);
            Node& n = nodes_[size_t(i)];
            n.height = 1 + std::max(nodes_[size_t(n.child1)].height, nodes_[size_t(n.child2)].height);
            n.box = unite(nodes_[size_t(n.child1)].box, nodes_[size_t(n.child2)].box);
            i = n.parent;
        }
    } else {
        root_ = sibling;
        nodes_[size_t(sibling)].parent = -1;
        release(parent);
    }
    nodes_[size_t(leaf)].parent = -1;
}

void SpatialIndex::insert(Key key, const Bounds& b, uint64_t group) {
    checkBounds(b);
    if (leafOf_.count(key)) fail("spatial key " + std::to_string(key) + " is already present");
    if (leafOf_.size() >= settings_.maxProxies) throw std::length_error("spatial index proxy limit reached");
    // Up to two new nodes (leaf + parent): make room first so nothing below can reallocate or throw.
    const size_t need = freeCount_ >= 2 ? 0 : 2 - freeCount_;
    if (nodes_.size() + need > nodes_.capacity()) nodes_.reserve(std::max(nodes_.capacity() * 2, nodes_.size() + need));
    auto [it, inserted] = leafOf_.try_emplace(key, -1);
    (void)inserted;
    const int32_t leaf = allocate();
    Node& n = nodes_[size_t(leaf)];
    n.key = key;
    n.group = group;
    n.tight = b;
    n.box = fatten(b);
    insertLeaf(leaf);
    it->second = leaf;
}

bool SpatialIndex::update(Key key, const Bounds& b) {
    auto it = leafOf_.find(key);
    if (it == leafOf_.end()) fail("unknown spatial key " + std::to_string(key));
    checkBounds(b);
    Node& n = nodes_[size_t(it->second)];
    if (n.box.contains(b)) {
        n.tight = b;
        return false;
    }
    removeLeaf(it->second);  // frees one internal node, which insertLeaf reuses: no allocation
    Node& m = nodes_[size_t(it->second)];
    m.tight = b;
    m.box = fatten(b);
    insertLeaf(it->second);
    return true;
}

bool SpatialIndex::remove(Key key) {
    auto it = leafOf_.find(key);
    if (it == leafOf_.end()) return false;
    const int32_t leaf = it->second;
    removeLeaf(leaf);
    release(leaf);
    leafOf_.erase(it);
    return true;
}

size_t SpatialIndex::removeGroup(uint64_t group) {
    std::vector<Key> keys;
    for (const auto& [key, leaf] : leafOf_)
        if (nodes_[size_t(leaf)].group == group) keys.push_back(key);
    std::sort(keys.begin(), keys.end());  // deterministic tree shape afterwards
    for (Key k : keys) remove(k);
    return keys.size();
}

void SpatialIndex::clear() {
    nodes_.clear();
    root_ = free_ = -1;
    freeCount_ = 0;
    leafOf_.clear();
}

const Bounds* SpatialIndex::bounds(Key key) const {
    auto it = leafOf_.find(key);
    return it == leafOf_.end() ? nullptr : &nodes_[size_t(it->second)].tight;
}

int SpatialIndex::height() const { return root_ < 0 ? 0 : nodes_[size_t(root_)].height; }

template <class Overlap, class Visit>
void SpatialIndex::traverse(Overlap&& overlap, Visit&& visit) const {
    if (root_ < 0) return;
    NodeStack stack;
    stack.push(root_);
    while (!stack.empty()) {
        const Node& n = nodes_[size_t(stack.pop())];
        if (!overlap(n.box)) continue;
        if (n.leaf()) {
            visit(n);
        } else {
            stack.push(n.child2);
            stack.push(n.child1);
        }
    }
}

void SpatialIndex::queryBox(const Bounds& box, std::vector<Key>& out) const {
    checkQueryBox(box);
    out.clear();
    traverse([&](const Bounds& b) { return b.overlaps(box); },
             [&](const Node& n) {
                 if (n.tight.overlaps(box)) out.push_back(n.key);
             });
    std::sort(out.begin(), out.end());
}

void SpatialIndex::querySphere(const DVec3& c, double radius, std::vector<Key>& out) const {
    if (!finite(c) || !(radius >= 0) || !std::isfinite(radius)) fail("sphere query needs a finite centre and radius >= 0");
    out.clear();
    const double r2 = radius * radius;
    traverse([&](const Bounds& b) { return distanceSquared(c, b) <= r2; },
             [&](const Node& n) {
                 if (distanceSquared(c, n.tight) <= r2) out.push_back(n.key);
             });
    std::sort(out.begin(), out.end());
}

void SpatialIndex::queryRay(const DVec3& o, const DVec3& dir, double maxDistance, std::vector<RayHit>& out) const {
    const double len = std::sqrt(dot(dir, dir));
    if (!finite(o) || !finite(dir) || !(len > 0) || !std::isfinite(len)) fail("ray needs a finite origin and non-zero direction");
    if (!(maxDistance >= 0) || !std::isfinite(maxDistance)) fail("ray maxDistance must be finite and >= 0");
    const DVec3 d{dir.x / len, dir.y / len, dir.z / len};
    out.clear();
    double t = 0;
    traverse([&](const Bounds& b) { return rayHitsBox(o, d, maxDistance, b, t); },
             [&](const Node& n) {
                 if (rayHitsBox(o, d, maxDistance, n.tight, t)) out.push_back({n.key, t});
             });
    std::sort(out.begin(), out.end(), [](const RayHit& a, const RayHit& b) {
        return a.distance != b.distance ? a.distance < b.distance : a.key < b.key;
    });
}

void SpatialIndex::queryFrustum(const Frustum& f, std::vector<Key>& out) const {
    for (const Plane& p : f.planes)
        if (!finite(p.normal) || !std::isfinite(p.d)) fail("frustum planes must be finite");
    out.clear();
    traverse([&](const Bounds& b) { return !f.outside(b); },
             [&](const Node& n) {
                 if (!f.outside(n.tight)) out.push_back(n.key);
             });
    std::sort(out.begin(), out.end());
}

void SpatialIndex::validate() const {
    auto check = [](bool ok, const char* what) {
        if (!ok) throw std::logic_error(std::string("spatial index invariant violated: ") + what);
    };
    size_t reachable = 0, leaves = 0;
    if (root_ >= 0) {
        check(nodes_[size_t(root_)].parent == -1, "root has a parent");
        std::vector<int32_t> stack{root_};
        while (!stack.empty()) {
            const int32_t i = stack.back();
            stack.pop_back();
            check(i >= 0 && size_t(i) < nodes_.size(), "node index out of range");
            const Node& n = nodes_[size_t(i)];
            check(n.height >= 0, "free node reachable from root");
            ++reachable;
            if (n.leaf()) {
                ++leaves;
                check(n.height == 0, "leaf height");
                check(n.box.contains(n.tight), "fat bounds do not contain tight bounds");
                auto it = leafOf_.find(n.key);
                check(it != leafOf_.end() && it->second == i, "leaf map mismatch");
                continue;
            }
            check(n.child2 >= 0, "internal node with one child");
            const Node& a = nodes_[size_t(n.child1)];
            const Node& b = nodes_[size_t(n.child2)];
            check(a.parent == i && b.parent == i, "child parent link");
            check(n.height == 1 + std::max(a.height, b.height), "internal height");
            check(n.box == unite(a.box, b.box), "internal bounds are not the union of the children");
            stack.push_back(n.child1);
            stack.push_back(n.child2);
        }
    }
    check(leaves == leafOf_.size(), "leaf count");
    size_t freeNodes = 0;
    for (int32_t i = free_; i >= 0; i = nodes_[size_t(i)].parent) {
        check(nodes_[size_t(i)].height == -1, "free list node in use");
        check(++freeNodes <= nodes_.size(), "free list cycle");
    }
    check(freeNodes == freeCount_, "free count");
    check(reachable + freeNodes == nodes_.size(), "leaked nodes");
}

// ---- streaming helpers -------------------------------------------------------------------------------------------

Cell cellOf(const DVec3& p, double cellSize) {
    if (!(cellSize > 0) || !std::isfinite(cellSize) || !finite(p)) fail("cellOf needs finite input and cellSize > 0");
    auto idx = [&](double v) {
        const double c = std::floor(v / cellSize);
        if (!inRange(c, -9.0e18, 9.0e18)) fail("cell index out of range");
        return int64_t(c);
    };
    return {idx(p.x), idx(p.y), idx(p.z)};
}

void cellsOverlapping(const Bounds& b, double cellSize, std::vector<Cell>& out, size_t maxCells) {
    checkQueryBox(b);
    const Cell lo = cellOf(b.min, cellSize), hi = cellOf(b.max, cellSize);  // hi >= lo: floor is monotonic
    // Spans in unsigned arithmetic: the int64 subtraction hi - lo can overflow (|index| may reach 9e18), the
    // unsigned difference of the two's-complement values cannot (it is exact for every hi >= lo).
    auto span = [](int64_t l, int64_t h) { return uint64_t(h) - uint64_t(l); };
    const uint64_t sx = span(lo.x, hi.x), sy = span(lo.y, hi.y), sz = span(lo.z, hi.z);
    const uint64_t limit = std::min<uint64_t>(maxCells, out.max_size());  // never ask for more than a vector can hold
    // Checked product of (s + 1) per axis without ever forming an overflowing value.
    if (sx >= limit || sy >= limit || sz >= limit) throw std::length_error("bounds cover more than maxCells streaming cells");
    const uint64_t nx = sx + 1, ny = sy + 1, nz = sz + 1;
    if (nx > limit / ny || nx * ny > limit / nz) throw std::length_error("bounds cover more than maxCells streaming cells");
    const size_t count = size_t(nx * ny * nz);
    out.reserve(count);  // may throw; `out` is untouched until here
    out.clear();
    // Offsets from lo never exceed the (small) spans, so lo + offset cannot overflow and loops end without ++ past hi.
    for (uint64_t i = 0; i < nx; ++i)
        for (uint64_t j = 0; j < ny; ++j)
            for (uint64_t k = 0; k < nz; ++k)
                out.push_back({lo.x + int64_t(i), lo.y + int64_t(j), lo.z + int64_t(k)});
}

}  // namespace titan::spatial
