// TITAN Engine — spatial world index: a dynamic bounding-volume tree for large, changing scenes.
// Original TITAN code.
//
// Structure choice. Scenes mix tiny props and kilometre-scale terrain and move constantly, and open worlds have no
// fixed bounds. A uniform grid or octree needs a cell size and world box chosen up front and degrades with mixed
// object sizes; a dynamic AABB tree has neither problem: O(log n) insert/remove, O(log n + k) box queries, memory
// linear in the object count (2n - 1 nodes), and no world bounds. Leaves keep a "fat" AABB (tight bounds grown by
// a margin) so small movements update no tree structure at all. The tree is kept height-balanced with AVL-style
// rotations; insertion picks siblings by a surface-area cost.
//
// Precision. Bounds are double precision so coordinates far from the origin (open worlds, planetary scales up to
// Settings::maxCoordinate, default 1e12 m) keep sub-millimetre resolution. Float scene data converts losslessly.
//
// Contracts:
//  * Keys are caller-chosen stable identifiers (e.g. ObjectId); a key is present at most once.
//  * Bounds must be finite, min <= max on every axis (zero-size boxes are points) and inside +/- maxCoordinate;
//    anything else throws std::invalid_argument and changes nothing.
//  * Query results are deterministic: key lists ascend, ray hits ascend by (entry distance, key). Results are
//    against the tight bounds; touching (shared boundary) counts as overlapping.
//  * Frustum culling is the standard conservative plane test: a box is rejected only when it lies entirely behind
//    one plane (boxes near frustum corners may be reported).
//  * Groups: every proxy carries a caller-defined group (e.g. a streaming cell). removeGroup() drops a whole group,
//    which is how a world-streaming layer unloads a cell. cellsOverlapping() maps bounds to cells.
//
// Thread safety: none for mutations. Const queries may run concurrently with each other (they use no shared
// scratch) but never with insert / update / remove.
// Not exposed over the protocol yet: no capability is advertised until it is wired and verified.
#pragma once

#include "titan/math.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace titan::spatial {

struct DVec3 {
    double x = 0, y = 0, z = 0;
    constexpr DVec3() = default;
    constexpr DVec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    explicit DVec3(const Vec3& v) : x(v.x), y(v.y), z(v.z) {}
    bool operator==(const DVec3& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct Bounds {
    DVec3 min, max;
    static Bounds around(const DVec3& centre, const DVec3& halfExtents);
    bool overlaps(const Bounds& o) const {
        return min.x <= o.max.x && o.min.x <= max.x && min.y <= o.max.y && o.min.y <= max.y && min.z <= o.max.z &&
               o.min.z <= max.z;
    }
    bool contains(const Bounds& o) const {
        return min.x <= o.min.x && min.y <= o.min.y && min.z <= o.min.z && o.max.x <= max.x && o.max.y <= max.y &&
               o.max.z <= max.z;
    }
    bool operator==(const Bounds& o) const { return min == o.min && max == o.max; }
};

using Key = uint64_t;

struct Plane {
    DVec3 normal;  // inside is where dot(normal, p) + d >= 0
    double d = 0;
};

struct Frustum {
    enum class ClipDepth { MinusOneToOne, ZeroToOne };
    std::array<Plane, 6> planes;  // left, right, bottom, top, near, far
    // Planes of a column-major view-projection matrix (Gribb-Hartmann). TITAN's Mat4::perspective is OpenGL style
    // (MinusOneToOne). Throws std::invalid_argument for a non-finite or degenerate matrix.
    static Frustum fromViewProjection(const Mat4& viewProjection, ClipDepth depth = ClipDepth::MinusOneToOne);
    bool outside(const Bounds& b) const;
};

struct RayHit {
    Key key = 0;
    double distance = 0;  // where the ray enters the tight bounds (0 when it starts inside)
};

struct Settings {
    double margin = 0.1;          // absolute fat-bounds margin (world units), [0, 1e6]
    double marginFraction = 0.1;  // plus this fraction of each axis' extent, [0, 10]
    size_t maxProxies = size_t(1) << 22;
    double maxCoordinate = 1e12;  // (0, 1e15]
};

class SpatialIndex {
public:
    explicit SpatialIndex(Settings settings = {});  // std::invalid_argument for invalid settings

    // std::invalid_argument: invalid bounds or duplicate key; std::length_error: maxProxies reached.
    void insert(Key key, const Bounds& bounds, uint64_t group = 0);
    // Returns true when the tree structure changed (bounds left the fat box). std::invalid_argument for an unknown
    // key or invalid bounds (nothing changes).
    bool update(Key key, const Bounds& bounds);
    bool remove(Key key);
    size_t removeGroup(uint64_t group);  // O(n) scan; returns the number removed
    void clear();

    bool contains(Key key) const { return leafOf_.count(key) != 0; }
    const Bounds* bounds(Key key) const;  // tight bounds
    size_t size() const { return leafOf_.size(); }
    int height() const;  // 0 when empty or a single leaf

    // Each query clears `out` and fills it; the caller may reuse the vector to avoid allocations.
    void queryBox(const Bounds& box, std::vector<Key>& out) const;
    void querySphere(const DVec3& centre, double radius, std::vector<Key>& out) const;
    // Candidates along origin + t * direction for t in [0, maxDistance]; direction need not be normalized
    // (distances are in units of the normalized direction).
    void queryRay(const DVec3& origin, const DVec3& direction, double maxDistance, std::vector<RayHit>& out) const;
    void queryFrustum(const Frustum& frustum, std::vector<Key>& out) const;

    // Checks every structural invariant (parent links, heights, containment, leaf map); throws std::logic_error.
    void validate() const;
    size_t nodeCapacity() const { return nodes_.capacity(); }

private:
    struct Node {
        Bounds box;     // fat bounds for leaves, union of children otherwise
        Bounds tight;   // leaves only
        Key key = 0;
        uint64_t group = 0;
        int32_t parent = -1;  // next free node while on the free list
        int32_t child1 = -1, child2 = -1;
        int32_t height = 0;  // 0 for leaves, -1 when free
        bool leaf() const { return child1 < 0; }
    };

    void checkBounds(const Bounds& b) const;
    Bounds fatten(const Bounds& b) const;
    int32_t allocate();
    void release(int32_t i);
    void insertLeaf(int32_t leaf);
    void removeLeaf(int32_t leaf);
    int32_t balance(int32_t a);
    template <class Overlap, class Visit>
    void traverse(Overlap&& overlap, Visit&& visit) const;

    Settings settings_;
    std::vector<Node> nodes_;
    int32_t root_ = -1;
    int32_t free_ = -1;
    size_t freeCount_ = 0;
    std::unordered_map<Key, int32_t> leafOf_;
};

// World-streaming helpers: integer cell coordinates of a regular grid with the given cell size.
struct Cell {
    int64_t x = 0, y = 0, z = 0;
    bool operator==(const Cell& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator<(const Cell& o) const { return x != o.x ? x < o.x : (y != o.y ? y < o.y : z < o.z); }
};
Cell cellOf(const DVec3& p, double cellSize);  // std::invalid_argument for non-finite input or cellSize <= 0
// Cells touched by `b` in ascending (x, y, z) order. std::length_error when more than maxCells would be produced
// (checked before any allocation, for any span up to the full +/-9e18 index range); `out` is unchanged on failure.
void cellsOverlapping(const Bounds& b, double cellSize, std::vector<Cell>& out, size_t maxCells = 4096);

}  // namespace titan::spatial
