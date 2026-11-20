// TITAN spatial world index tests: structural invariants and agreement with brute force. Original TITAN code.
#include "titan/spatial.h"
#include "titan_test.h"

#include <algorithm>
#include <limits>
#include <map>

using namespace titan;
using namespace titan::spatial;

namespace {
const double kNan = std::numeric_limits<double>::quiet_NaN();

struct Rng {
    uint64_t s;
    double next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return double(s >> 11) * (1.0 / 9007199254740992.0);
    }
    double range(double a, double b) { return a + (b - a) * next(); }
};

// Independent brute-force reference.
struct Reference {
    std::map<Key, Bounds> items;
    std::vector<Key> box(const Bounds& q) const {
        std::vector<Key> out;
        for (const auto& [k, b] : items)
            if (b.overlaps(q)) out.push_back(k);
        return out;
    }
    std::vector<Key> sphere(const DVec3& c, double r) const {
        std::vector<Key> out;
        for (const auto& [k, b] : items) {
            const double dx = std::max({b.min.x - c.x, 0.0, c.x - b.max.x});
            const double dy = std::max({b.min.y - c.y, 0.0, c.y - b.max.y});
            const double dz = std::max({b.min.z - c.z, 0.0, c.z - b.max.z});
            if (dx * dx + dy * dy + dz * dz <= r * r) out.push_back(k);
        }
        return out;
    }
    std::vector<RayHit> ray(const DVec3& o, const DVec3& dir, double maxT) const {
        const double len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        const double d[3] = {dir.x / len, dir.y / len, dir.z / len};
        const double oc[3] = {o.x, o.y, o.z};
        std::vector<RayHit> out;
        for (const auto& [k, b] : items) {
            const double lo[3] = {b.min.x, b.min.y, b.min.z}, hi[3] = {b.max.x, b.max.y, b.max.z};
            double tmin = 0, tmax = maxT;
            bool hit = true;
            for (int i = 0; i < 3 && hit; ++i) {
                if (std::fabs(d[i]) < 1e-300) {
                    hit = !(oc[i] < lo[i] || oc[i] > hi[i]);
                    continue;
                }
                const double inv = 1.0 / d[i];
                double t1 = (lo[i] - oc[i]) * inv, t2 = (hi[i] - oc[i]) * inv;
                if (t1 > t2) std::swap(t1, t2);
                tmin = std::max(tmin, t1);
                tmax = std::min(tmax, t2);
                hit = tmin <= tmax;
            }
            if (hit) out.push_back({k, tmin});
        }
        std::sort(out.begin(), out.end(), [](const RayHit& a, const RayHit& b) {
            return a.distance != b.distance ? a.distance < b.distance : a.key < b.key;
        });
        return out;
    }
    std::vector<Key> frustum(const Frustum& f) const {
        std::vector<Key> out;
        for (const auto& [k, b] : items)
            if (!f.outside(b)) out.push_back(k);
        return out;
    }
};

bool sameHits(const std::vector<RayHit>& a, const std::vector<RayHit>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        // Same arithmetic, but allow for floating-point contraction differences between the two code paths.
        if (a[i].key != b[i].key || std::fabs(a[i].distance - b[i].distance) > 1e-9 * (1 + b[i].distance)) return false;
    return true;
}

Bounds cube(double x, double y, double z, double h) { return Bounds::around({x, y, z}, {h, h, h}); }
}  // namespace

TEST("spatial: invalid input is refused and changes nothing") {
    Settings bad;
    bad.margin = -1;
    CHECK_THROWS(SpatialIndex(bad));
    bad = {};
    bad.maxCoordinate = 0;
    CHECK_THROWS(SpatialIndex(bad));
    bad = {};
    bad.maxProxies = 0;
    CHECK_THROWS(SpatialIndex(bad));

    Settings s;
    s.maxProxies = 2;
    SpatialIndex idx(s);
    CHECK_THROWS(idx.insert(1, Bounds{{kNan, 0, 0}, {1, 1, 1}}));
    CHECK_THROWS(idx.insert(1, Bounds{{0, 0, 0}, {std::numeric_limits<double>::infinity(), 1, 1}}));
    CHECK_THROWS(idx.insert(1, Bounds{{1, 0, 0}, {0, 1, 1}}));     // inverted
    CHECK_THROWS(idx.insert(1, Bounds{{0, 0, 0}, {2e12, 1, 1}}));  // beyond maxCoordinate
    CHECK(idx.size() == 0);
    idx.validate();
    idx.insert(1, cube(0, 0, 0, 1));
    CHECK_THROWS(idx.insert(1, cube(5, 5, 5, 1)));  // duplicate key
    idx.insert(2, cube(5, 5, 5, 1));
    CHECK_THROWS(idx.insert(3, cube(9, 9, 9, 1)));  // proxy limit
    CHECK_THROWS(idx.update(99, cube(0, 0, 0, 1)));
    CHECK_THROWS(idx.update(1, Bounds{{0, 0, 0}, {kNan, 1, 1}}));
    CHECK(*idx.bounds(1) == cube(0, 0, 0, 1));
    CHECK(!idx.remove(99));
    std::vector<Key> keys;
    std::vector<RayHit> hits;
    CHECK_THROWS(idx.queryBox(Bounds{{1, 0, 0}, {0, 1, 1}}, keys));
    CHECK_THROWS(idx.querySphere({0, 0, 0}, -1, keys));
    CHECK_THROWS(idx.queryRay({0, 0, 0}, {0, 0, 0}, 10, hits));
    CHECK_THROWS(idx.queryRay({kNan, 0, 0}, {1, 0, 0}, 10, hits));
    CHECK_THROWS(idx.queryRay({0, 0, 0}, {1, 0, 0}, kNan, hits));
    idx.validate();
    CHECK(idx.size() == 2);
}

TEST("spatial: randomized inserts, moves and removals agree with brute force at large coordinates") {
    SpatialIndex idx;
    Reference ref;
    Rng rng{12345};
    const DVec3 base{1e9, -1e9, 5e8};
    auto randomBounds = [&] {
        const DVec3 c{base.x + rng.range(-5000, 5000), base.y + rng.range(-5000, 5000), base.z + rng.range(-5000, 5000)};
        const double cls = rng.next();
        const double h = cls < 0.3 ? 0.0 : (cls < 0.8 ? rng.range(0.01, 5) : rng.range(5, 500));
        return Bounds::around(c, {h, h * rng.range(0.2, 1), h * rng.range(0.2, 1)});
    };
    Key nextKey = 1000;
    for (int i = 0; i < 3000; ++i) {
        const Bounds b = randomBounds();
        idx.insert(nextKey, b);
        ref.items[nextKey] = b;
        nextKey += 7;
    }
    auto compare = [&] {
        idx.validate();
        CHECK(idx.size() == ref.items.size());
        const double n = double(idx.size());
        CHECK(double(idx.height()) <= 3 * std::log2(n) + 2);
        std::vector<Key> got;
        std::vector<RayHit> hits;
        for (int q = 0; q < 150; ++q) {
            const DVec3 c{base.x + rng.range(-6000, 6000), base.y + rng.range(-6000, 6000), base.z + rng.range(-6000, 6000)};
            const double h = q % 10 == 0 ? 0.0 : rng.range(1, 1500);
            const Bounds qb = Bounds::around(c, {h, h * 0.5, h * 2});
            idx.queryBox(qb, got);
            CHECK(got == ref.box(qb));
            const double r = rng.range(0, 2000);
            idx.querySphere(c, r, got);
            CHECK(got == ref.sphere(c, r));
            const DVec3 dir{rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)};
            const double maxT = rng.range(0, 20000);
            idx.queryRay(c, dir, maxT, hits);
            CHECK(sameHits(hits, ref.ray(c, dir, maxT)));
        }
        for (int q = 0; q < 20; ++q) {
            const Vec3 target{float(rng.range(-1, 1)), float(rng.range(-1, 1)), float(rng.range(-1, 1))};
            Frustum f = Frustum::fromViewProjection(Mat4::perspective(60, 1.5f, 0.5f, 4000) *
                                                    Mat4::lookAt({0, 0, 0}, target, {0, 1, 0}));
            for (Plane& p : f.planes) p.d -= p.normal.x * base.x + p.normal.y * base.y + p.normal.z * base.z;  // move to base
            idx.queryFrustum(f, got);
            CHECK(got == ref.frustum(f));
        }
    };
    compare();
    int structural = 0, inPlace = 0;
    std::vector<Key> live;
    for (int op = 0; op < 4000; ++op) {
        live.clear();
        for (const auto& [k, b] : ref.items) live.push_back(k);
        const Key k = live[size_t(rng.next() * double(live.size())) % live.size()];
        const double roll = rng.next();
        if (roll < 0.6) {
            Bounds b = ref.items[k];
            const DVec3 d{rng.range(-0.05, 0.05), rng.range(-0.05, 0.05), rng.range(-0.05, 0.05)};
            b.min = {b.min.x + d.x, b.min.y + d.y, b.min.z + d.z};
            b.max = {b.max.x + d.x, b.max.y + d.y, b.max.z + d.z};
            (idx.update(k, b) ? structural : inPlace)++;
            ref.items[k] = b;
        } else if (roll < 0.8) {
            const Bounds b = randomBounds();
            (idx.update(k, b) ? structural : inPlace)++;
            ref.items[k] = b;
        } else if (roll < 0.9) {
            CHECK(idx.remove(k));
            ref.items.erase(k);
        } else {
            const Bounds b = randomBounds();
            idx.insert(nextKey, b);
            ref.items[nextKey] = b;
            nextKey += 7;
        }
    }
    CHECK(inPlace > 0 && structural > 0);  // fat bounds absorb small moves; large moves restructure
    compare();
}

TEST("spatial: large coordinates keep millimetre-scale objects distinct") {
    SpatialIndex idx;
    const double x0 = 1e11;
    for (Key i = 0; i < 50; ++i) {
        const double x = x0 + 2e-3 * double(i);
        idx.insert(i, Bounds{{x, 1e11, -1e11}, {x + 1e-3, 1e11 + 1e-3, -1e11 + 1e-3}});
    }
    std::vector<Key> got;
    const double x5 = x0 + 2e-3 * 5;
    idx.queryBox(Bounds{{x5 + 1e-4, 1e11, -1e11}, {x5 + 9e-4, 1e11 + 1e-3, -1e11 + 1e-3}}, got);
    CHECK(got.size() == 1 && got[0] == 5);
    const double x6 = x0 + 2e-3 * 6;  // same expression as the insert, so the boundary is bit-identical
    idx.queryBox(Bounds{{x5 + 1e-3, 1e11, -1e11}, {x6, 1e11, -1e11}}, got);  // touches 5's max and 6's min
    const std::vector<Key> both{5, 6};
    CHECK(got == both);
    idx.validate();
}

TEST("spatial: ray ordering, ties, inside starts, point bounds and touching boxes") {
    SpatialIndex idx;
    idx.insert(9, Bounds{{5, -1, -1}, {6, 1, 1}});
    idx.insert(7, Bounds{{10, -1, -1}, {11, 1, 1}});
    idx.insert(3, Bounds{{10, -2, -1}, {12, 2, 1}});
    idx.insert(11, Bounds{{20, 0, 0}, {20, 0, 0}});  // a point
    std::vector<RayHit> hits;
    idx.queryRay({0, 0, 0}, {2, 0, 0}, 100, hits);  // unnormalized direction
    CHECK(hits.size() == 4);
    CHECK(hits[0].key == 9 && hits[0].distance == 5);
    CHECK(hits[1].key == 3 && hits[2].key == 7 && hits[1].distance == 10 && hits[2].distance == 10);  // tie -> key
    CHECK(hits[3].key == 11 && hits[3].distance == 20);
    idx.queryRay({0, 0, 0}, {1, 0, 0}, 7, hits);
    CHECK(hits.size() == 1 && hits[0].key == 9);
    idx.queryRay({0, 0, 0}, {1, 0, 0}, 5, hits);  // reaching exactly the entry distance counts
    CHECK(hits.size() == 1);
    idx.queryRay({5.5, 0, 0}, {1, 0, 0}, 100, hits);
    CHECK(hits[0].key == 9 && hits[0].distance == 0);
    std::vector<Key> got;
    idx.queryBox(Bounds{{20, 0, 0}, {21, 1, 1}}, got);  // shares the point
    CHECK(got.size() == 1 && got[0] == 11);
    idx.querySphere({20, 0, 3}, 3, got);  // exactly at the radius
    CHECK(got.size() == 1 && got[0] == 11);
    idx.querySphere({20, 0, 3}, 2.999, got);
    CHECK(got.empty());
}

TEST("spatial: updates inside the fat bounds keep the structure; queries use tight bounds") {
    Settings s;
    s.margin = 1;
    s.marginFraction = 0;
    SpatialIndex idx(s);
    idx.insert(1, Bounds{{0, 0, 0}, {1, 1, 1}});
    idx.insert(2, Bounds{{50, 0, 0}, {51, 1, 1}});
    CHECK(!idx.update(1, Bounds{{0.5, 0, 0}, {1.5, 1, 1}}));
    CHECK(*idx.bounds(1) == (Bounds{{0.5, 0, 0}, {1.5, 1, 1}}));
    std::vector<Key> got;
    idx.queryBox(Bounds{{-0.5, 0, 0}, {0.2, 1, 1}}, got);  // overlaps the old and fat bounds, not the tight ones
    CHECK(got.empty());
    CHECK(idx.update(1, Bounds{{10, 0, 0}, {11, 1, 1}}));
    idx.queryBox(Bounds{{10.5, 0.5, 0.5}, {10.5, 0.5, 0.5}}, got);
    CHECK(got.size() == 1 && got[0] == 1);
    idx.validate();
    SpatialIndex single(s);
    single.insert(4, cube(0, 0, 0, 1));
    CHECK(single.update(4, cube(100, 0, 0, 1)));  // root-only tree
    single.validate();
}

TEST("spatial: frustum planes from TITAN's view-projection convention") {
    const Mat4 vp = Mat4::perspective(90, 1, 1, 100) * Mat4::lookAt({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
    const Frustum f = Frustum::fromViewProjection(vp);
    SpatialIndex idx;
    idx.insert(1, cube(0, 0, -10, 0.5));
    idx.insert(2, cube(0, 0, 10, 0.5));    // behind the camera
    idx.insert(3, cube(0, 0, -200, 0.5));  // beyond the far plane
    idx.insert(4, cube(50, 0, -10, 0.5));  // outside the right plane
    idx.insert(5, cube(-5, 0, -10, 0.5));
    std::vector<Key> got;
    idx.queryFrustum(f, got);
    const std::vector<Key> want{1, 5};
    CHECK(got == want);
    Mat4 broken = vp;
    broken.at(0, 0) = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS(Frustum::fromViewProjection(broken));
    Mat4 zero;
    zero.m.fill(0);
    CHECK_THROWS(Frustum::fromViewProjection(zero));
}

TEST("spatial: groups support streaming unloads; clear resets") {
    SpatialIndex idx;
    for (Key i = 0; i < 100; ++i) idx.insert(i, cube(double(i), 0, 0, 0.4), i % 4);
    CHECK(idx.removeGroup(2) == 25);
    CHECK(idx.removeGroup(99) == 0);
    idx.validate();
    std::vector<Key> got;
    idx.queryBox(Bounds{{-1e3, -1e3, -1e3}, {1e3, 1e3, 1e3}}, got);
    CHECK(got.size() == 75);
    CHECK(std::none_of(got.begin(), got.end(), [](Key k) { return k % 4 == 2; }));
    idx.clear();
    CHECK(idx.size() == 0 && idx.height() == 0);
    idx.validate();
    idx.insert(7, cube(0, 0, 0, 1));
    CHECK(idx.contains(7));
}

TEST("spatial: streaming cell helpers") {
    const Cell c = cellOf({-0.5, 0, 9.99}, 10);
    CHECK(c == (Cell{-1, 0, 0}));
    std::vector<Cell> cells;
    cellsOverlapping(Bounds{{-1, -1, -1}, {1, 1, 1}}, 1, cells);
    CHECK(cells.size() == 27);
    CHECK(cells.front() == (Cell{-1, -1, -1}) && cells.back() == (Cell{1, 1, 1}));
    CHECK(std::is_sorted(cells.begin(), cells.end()));
    CHECK_THROWS(cellsOverlapping(Bounds{{-1, -1, -1}, {1, 1, 1}}, 1, cells, 26));
    CHECK_THROWS(cellOf({0, 0, 0}, 0));
    CHECK_THROWS(cellOf({kNan, 0, 0}, 1));
    CHECK_THROWS(cellOf({1e300, 0, 0}, 1e-10));
}

TEST("spatial: extreme cell ranges are refused before allocating, without signed overflow (PR #32 review)") {
    std::vector<Cell> cells{{7, 7, 7}};
    const std::vector<Cell> sentinel = cells;
    // Each endpoint index fits int64, but hi - lo (1.6e19) does not: must be a clean length_error.
    CHECK_THROWS(cellsOverlapping(Bounds{{-8e18, 0, 0}, {8e18, 0, 0}}, 1, cells));
    CHECK_THROWS(cellsOverlapping(Bounds{{0, -8.9e18, 0}, {0, 8.9e18, 0}}, 1, cells));
    CHECK_THROWS(cellsOverlapping(Bounds{{0, 0, -8.9e18}, {0, 0, 8.9e18}}, 1, cells, size_t(-1)));
    // Spans that fit per axis but whose product exceeds the cap (and would overflow 64-bit if multiplied naively).
    CHECK_THROWS(cellsOverlapping(Bounds{{0, 0, 0}, {4e9, 4e9, 4e9}}, 1, cells, size_t(-1) / 2));
    CHECK_THROWS(cellsOverlapping(Bounds{{0, 0, 0}, {63, 63, 63}}, 1, cells, 4096 * 64 - 1));
    CHECK(cells == sentinel);  // output preserved on every failure
    // Extreme but tiny ranges near both ends of the index space still work.
    cellsOverlapping(Bounds{{-8.9e18, 8.9e18, 0}, {-8.9e18, 8.9e18, 0}}, 1, cells);
    CHECK(cells.size() == 1 && cells[0].x == int64_t(-8.9e18) && cells[0].y == int64_t(8.9e18));
    cellsOverlapping(Bounds{{0, 0, 0}, {63, 63, 0}}, 1, cells, 4096);
    CHECK(cells.size() == 4096 && cells.back() == (Cell{63, 63, 0}));
}

TITAN_TEST_MAIN()
