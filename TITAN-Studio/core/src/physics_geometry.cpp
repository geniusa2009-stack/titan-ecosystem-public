// TITAN Engine — physics geometry. Original TITAN code.
#include "titan/physics_geometry.h"

#include <algorithm>
#include <cmath>

namespace titan::physics {
namespace {

constexpr float kEps = 1e-12f;
const Vec3 kUp{0, 1, 0};
const Vec3 kAxes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

float comp(const Vec3& v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }
void setComp(Vec3& v, int i, float s) { (i == 0 ? v.x : (i == 1 ? v.y : v.z)) = s; }
float sq(const Vec3& v) { return dot(v, v); }

std::optional<ContactPoint> sphereSphere(const Vec3& ca, float ra, const Vec3& cb, float rb) {
    const Vec3 d = cb - ca;
    const float dist = length(d);
    const float r = ra + rb;
    if (dist > r) return std::nullopt;
    ContactPoint c;
    c.normal = dist > 1e-7f ? d * (1.0f / dist) : kUp;  // coincident centres: fixed, deterministic normal
    c.depth = r - dist;
    c.point = ca + c.normal * (ra - c.depth * 0.5f);
    return c;
}

std::optional<ContactPoint> sphereBox(const Vec3& c, float r, const Vec3& centre, const Quat& q, const Vec3& h) {
    const Quat inv = q.conjugate();
    const Vec3 local = inv.rotate(c - centre);
    const Vec3 clamped{std::clamp(local.x, -h.x, h.x), std::clamp(local.y, -h.y, h.y), std::clamp(local.z, -h.z, h.z)};
    ContactPoint out;
    if (!(local == clamped)) {
        const Vec3 d = local - clamped;  // box surface -> sphere centre
        const float dist = length(d);
        if (dist > r) return std::nullopt;
        const Vec3 nLocal = d * (-1.0f / dist);  // sphere -> box
        out.normal = q.rotate(nLocal);
        out.depth = r - dist;
        const Vec3 surface = centre + q.rotate(clamped);
        out.point = surface - out.normal * (out.depth * 0.5f);
        return out;
    }
    // Centre inside (or on) the box: push out through the nearest face; ties pick the lowest axis.
    int axis = 0;
    float pen = h.x - std::fabs(local.x);
    for (int i = 1; i < 3; ++i) {
        const float p = comp(h, i) - std::fabs(comp(local, i));
        if (p < pen) {
            pen = p;
            axis = i;
        }
    }
    const float s = comp(local, axis) >= 0 ? 1.0f : -1.0f;
    out.normal = q.rotate(kAxes[axis] * -s);
    out.depth = r + pen;
    Vec3 face = local;
    setComp(face, axis, s * comp(h, axis));
    out.point = centre + q.rotate(face);
    return out;
}

// Signed distance from a box-local point to the box (negative inside).
float boxSignedDistance(const Vec3& p, const Vec3& h) {
    const Vec3 q{std::fabs(p.x) - h.x, std::fabs(p.y) - h.y, std::fabs(p.z) - h.z};
    const Vec3 outside{std::max(q.x, 0.0f), std::max(q.y, 0.0f), std::max(q.z, 0.0f)};
    return length(outside) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
}

std::optional<ContactPoint> capsuleBox(const Vec3& p0, const Vec3& p1, float r, const Vec3& centre, const Quat& q,
                                       const Vec3& h) {
    // The signed distance to a convex box is convex along the segment: golden-section search for its minimum,
    // a fixed number of iterations so the result is deterministic. Then resolve as a sphere at that point.
    const Quat inv = q.conjugate();
    const Vec3 a = inv.rotate(p0 - centre);
    const Vec3 b = inv.rotate(p1 - centre);
    auto f = [&](float t) { return boxSignedDistance(a + (b - a) * t, h); };
    constexpr float kG = 0.6180339887f;
    float lo = 0, hi = 1;
    float x1 = hi - kG * (hi - lo), x2 = lo + kG * (hi - lo);
    float f1 = f(x1), f2 = f(x2);
    for (int i = 0; i < 48; ++i) {
        if (f1 <= f2) {
            hi = x2; x2 = x1; f2 = f1;
            x1 = hi - kG * (hi - lo); f1 = f(x1);
        } else {
            lo = x1; x1 = x2; f1 = f2;
            x2 = lo + kG * (hi - lo); f2 = f(x2);
        }
    }
    float best = 0.5f * (lo + hi), fb = f(best);
    for (float t : {0.0f, 1.0f})
        if (const float ft = f(t); ft < fb) { fb = ft; best = t; }
    return sphereBox(p0 + (p1 - p0) * best, r, centre, q, h);
}

std::optional<ContactPoint> boxBox(const Placed& A, const Placed& B) {
    Vec3 ax[3], bx[3];
    for (int i = 0; i < 3; ++i) {
        ax[i] = A.rotation.rotate(kAxes[i]);
        bx[i] = B.rotation.rotate(kAxes[i]);
    }
    const Vec3& ha = A.shape.halfExtents;
    const Vec3& hb = B.shape.halfExtents;
    const Vec3 d = B.position - A.position;
    float best = INFINITY;
    Vec3 bestN = kUp;
    auto test = [&](Vec3 L, bool edge) -> bool {
        if (edge) {
            const float len = length(L);
            if (len < 1e-5f) return true;  // near-parallel edges: covered by the face axes
            L = L * (1.0f / len);
        }
        const float ra = ha.x * std::fabs(dot(ax[0], L)) + ha.y * std::fabs(dot(ax[1], L)) + ha.z * std::fabs(dot(ax[2], L));
        const float rb = hb.x * std::fabs(dot(bx[0], L)) + hb.y * std::fabs(dot(bx[1], L)) + hb.z * std::fabs(dot(bx[2], L));
        const float dist = dot(d, L);
        const float overlap = ra + rb - std::fabs(dist);
        if (overlap < 0) return false;
        // Prefer face axes: an edge axis must be clearly better to win (stable normals for resting boxes).
        if (edge ? overlap < best - 1e-4f : overlap < best) {
            best = overlap;
            bestN = dist < 0 ? -L : L;
        }
        return true;
    };
    for (int i = 0; i < 3; ++i)
        if (!test(ax[i], false)) return std::nullopt;
    for (int i = 0; i < 3; ++i)
        if (!test(bx[i], false)) return std::nullopt;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (!test(cross(ax[i], bx[j]), true)) return std::nullopt;
    ContactPoint c;
    c.normal = bestN;
    c.depth = best;
    const Vec3 pa = closestPointOnBox(B.position, A.position, A.rotation, ha);
    const Vec3 pb = closestPointOnBox(A.position, B.position, B.rotation, hb);
    c.point = (pa + pb) * 0.5f;
    return c;
}

void capsuleSegment(const Placed& p, Vec3& a, Vec3& b) {
    const Vec3 axis = p.rotation.rotate(kUp) * p.shape.halfHeight;
    a = p.position - axis;
    b = p.position + axis;
}

std::optional<RayHit> raySphere(const Vec3& c, float r, const Vec3& o, const Vec3& d, float maxDist) {
    const Vec3 m = o - c;
    const float b = dot(m, d);
    const float cc = dot(m, m) - r * r;
    if (cc <= 0) return RayHit{0, o, -d};
    if (b > 0) return std::nullopt;
    const float disc = b * b - cc;
    if (disc < 0) return std::nullopt;
    const float t = std::max(-b - std::sqrt(disc), 0.0f);
    if (t > maxDist) return std::nullopt;
    const Vec3 p = o + d * t;
    return RayHit{t, p, normalize(p - c)};
}

std::optional<RayHit> rayBox(const Placed& box, const Vec3& o, const Vec3& d, float maxDist) {
    const Quat inv = box.rotation.conjugate();
    const Vec3 lo = inv.rotate(o - box.position);
    const Vec3 ld = inv.rotate(d);
    const Vec3& h = box.shape.halfExtents;
    if (std::fabs(lo.x) <= h.x && std::fabs(lo.y) <= h.y && std::fabs(lo.z) <= h.z) return RayHit{0, o, -d};
    float tEnter = -INFINITY, tExit = INFINITY;
    int enterAxis = 0;
    float enterSign = 1;
    for (int i = 0; i < 3; ++i) {
        const float oi = comp(lo, i), di = comp(ld, i), hi = comp(h, i);
        if (std::fabs(di) < kEps) {
            if (oi < -hi || oi > hi) return std::nullopt;
            continue;
        }
        float t1 = (-hi - oi) / di, t2 = (hi - oi) / di;
        float sign = -1;  // entering through the -axis face
        if (t1 > t2) {
            std::swap(t1, t2);
            sign = 1;
        }
        if (t1 > tEnter) {
            tEnter = t1;
            enterAxis = i;
            enterSign = sign;
        }
        tExit = std::min(tExit, t2);
        if (tEnter > tExit) return std::nullopt;
    }
    if (tExit < 0 || tEnter < 0 || tEnter > maxDist) return std::nullopt;
    return RayHit{tEnter, o + d * tEnter, box.rotation.rotate(kAxes[enterAxis] * enterSign)};
}

std::optional<RayHit> rayCapsule(const Placed& cap, const Vec3& o, const Vec3& d, float maxDist) {
    Vec3 p0, p1;
    capsuleSegment(cap, p0, p1);
    const float r = cap.shape.radius;
    if (sq(o - closestPointOnSegment(o, p0, p1)) <= r * r) return RayHit{0, o, -d};
    std::optional<RayHit> best;
    auto consider = [&](const std::optional<RayHit>& h) {
        if (h && (!best || h->distance < best->distance)) best = h;
    };
    consider(raySphere(p0, r, o, d, maxDist));
    consider(raySphere(p1, r, o, d, maxDist));
    const float len = 2 * cap.shape.halfHeight;
    if (len > 0) {
        const Vec3 u = cap.rotation.rotate(kUp);
        const Vec3 m = o - p0;
        const Vec3 dp = d - u * dot(d, u);
        const Vec3 mp = m - u * dot(m, u);
        const float a = dot(dp, dp);
        if (a > kEps) {
            const float b = dot(mp, dp);
            const float c = dot(mp, mp) - r * r;
            const float disc = b * b - a * c;
            if (disc >= 0) {
                const float t = (-b - std::sqrt(disc)) / a;
                const float axial = dot(m + d * t, u);
                if (t >= 0 && t <= maxDist && axial >= 0 && axial <= len) {
                    const Vec3 p = o + d * t;
                    consider(RayHit{t, p, normalize(p - (p0 + u * axial))});
                }
            }
        }
    }
    return best;
}

}  // namespace

Quat Quat::axisAngle(const Vec3& a, float radians) {
    const float s = std::sin(radians * 0.5f);
    return {a.x * s, a.y * s, a.z * s, std::cos(radians * 0.5f)};
}

Quat Quat::fromEulerDegrees(const Vec3& deg) {
    return axisAngle({0, 1, 0}, radians(deg.y)) * axisAngle({1, 0, 0}, radians(deg.x)) *
           axisAngle({0, 0, 1}, radians(deg.z));
}

Quat Quat::operator*(const Quat& o) const {
    return {w * o.x + x * o.w + y * o.z - z * o.y, w * o.y - x * o.z + y * o.w + z * o.x,
            w * o.z + x * o.y - y * o.x + z * o.w, w * o.w - x * o.x - y * o.y - z * o.z};
}

float Quat::norm() const { return std::sqrt(x * x + y * y + z * z + w * w); }

Quat Quat::normalized() const {
    const float n = norm();
    if (!(n > 0) || !std::isfinite(n)) return {};
    return {x / n, y / n, z / n, w / n};
}

Vec3 Quat::rotate(const Vec3& v) const {
    const Vec3 u{x, y, z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * w + cross(u, t);
}

bool Quat::finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(w); }

bool Shape::operator==(const Shape& o) const {
    return kind == o.kind && radius == o.radius && halfExtents == o.halfExtents && halfHeight == o.halfHeight;
}

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

Aabb worldAabb(const Placed& p) {
    switch (p.shape.kind) {
        case ShapeKind::Sphere: {
            const Vec3 r{p.shape.radius, p.shape.radius, p.shape.radius};
            return {p.position - r, p.position + r};
        }
        case ShapeKind::Box: {
            const Vec3& h = p.shape.halfExtents;
            Vec3 e;
            for (int i = 0; i < 3; ++i) {
                const Vec3 axis = p.rotation.rotate(kAxes[i]) * comp(h, i);
                e = e + Vec3{std::fabs(axis.x), std::fabs(axis.y), std::fabs(axis.z)};
            }
            return {p.position - e, p.position + e};
        }
        case ShapeKind::Capsule: {
            Vec3 a, b;
            capsuleSegment(p, a, b);
            const Vec3 r{p.shape.radius, p.shape.radius, p.shape.radius};
            return {Vec3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)} - r,
                    Vec3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)} + r};
        }
    }
    return {p.position, p.position};
}

Vec3 closestPointOnSegment(const Vec3& p, const Vec3& a, const Vec3& b, float* tOut) {
    const Vec3 ab = b - a;
    const float l2 = sq(ab);
    float t = l2 > kEps ? std::clamp(dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    if (tOut) *tOut = t;
    return a + ab * t;
}

void closestPointsSegments(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2, Vec3& c1, Vec3& c2) {
    const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = sq(d1), e = sq(d2), f = dot(d2, r);
    float s = 0, t = 0;
    if (a <= kEps && e <= kEps) {
        s = t = 0;
    } else if (a <= kEps) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = dot(d1, r);
        if (e <= kEps) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = dot(d1, d2);
            const float denom = a * e - b * b;
            // Parallel segments: denom ~ 0, any s works; s = 0 keeps the result deterministic.
            s = denom > 1e-7f * a * e ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0) {
                t = 0;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1) {
                t = 1;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
}

Vec3 closestPointOnBox(const Vec3& p, const Vec3& centre, const Quat& q, const Vec3& h) {
    const Vec3 local = q.conjugate().rotate(p - centre);
    const Vec3 c{std::clamp(local.x, -h.x, h.x), std::clamp(local.y, -h.y, h.y), std::clamp(local.z, -h.z, h.z)};
    if (c == local) return p;
    return centre + q.rotate(c);
}

std::optional<ContactPoint> collide(const Placed& a, const Placed& b) {
    auto flipped = [](std::optional<ContactPoint> c) {
        if (c) c->normal = -c->normal;
        return c;
    };
    const ShapeKind ka = a.shape.kind, kb = b.shape.kind;
    if (ka == ShapeKind::Sphere && kb == ShapeKind::Sphere)
        return sphereSphere(a.position, a.shape.radius, b.position, b.shape.radius);
    if (ka == ShapeKind::Sphere && kb == ShapeKind::Box)
        return sphereBox(a.position, a.shape.radius, b.position, b.rotation, b.shape.halfExtents);
    if (ka == ShapeKind::Box && kb == ShapeKind::Sphere) return flipped(collide(b, a));
    if (ka == ShapeKind::Sphere && kb == ShapeKind::Capsule) {
        Vec3 p0, p1;
        capsuleSegment(b, p0, p1);
        return sphereSphere(a.position, a.shape.radius, closestPointOnSegment(a.position, p0, p1), b.shape.radius);
    }
    if (ka == ShapeKind::Capsule && kb == ShapeKind::Sphere) return flipped(collide(b, a));
    if (ka == ShapeKind::Capsule && kb == ShapeKind::Capsule) {
        Vec3 a0, a1, b0, b1, ca, cb;
        capsuleSegment(a, a0, a1);
        capsuleSegment(b, b0, b1);
        closestPointsSegments(a0, a1, b0, b1, ca, cb);
        return sphereSphere(ca, a.shape.radius, cb, b.shape.radius);
    }
    if (ka == ShapeKind::Capsule && kb == ShapeKind::Box) {
        Vec3 p0, p1;
        capsuleSegment(a, p0, p1);
        return capsuleBox(p0, p1, a.shape.radius, b.position, b.rotation, b.shape.halfExtents);
    }
    if (ka == ShapeKind::Box && kb == ShapeKind::Capsule) return flipped(collide(b, a));
    return boxBox(a, b);
}

std::optional<RayHit> raycast(const Placed& p, const Vec3& origin, const Vec3& dir, float maxDistance) {
    switch (p.shape.kind) {
        case ShapeKind::Sphere: return raySphere(p.position, p.shape.radius, origin, dir, maxDistance);
        case ShapeKind::Box: return rayBox(p, origin, dir, maxDistance);
        case ShapeKind::Capsule: return rayCapsule(p, origin, dir, maxDistance);
    }
    return std::nullopt;
}

}  // namespace titan::physics
