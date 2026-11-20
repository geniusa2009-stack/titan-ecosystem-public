// TITAN Engine — physics geometry: rotations, collision primitives, contact generation and ray casts.
// Original TITAN code.
//
// Engine-independent, allocation-free utilities shared by the reference simulation (titan/physics.h), editor
// tooling and the future Chaos bridge (which maps the same primitives to Chaos implicit objects).
//
// Conventions:
//  * Right-handed, Y-up, metres, single precision (titan::Vec3). Rotations are unit quaternions; Euler input uses
//    the scene convention R = Ry * Rx * Rz (titan/math.h).
//  * A capsule is a segment along its local Y axis of half-length `halfHeight`, swept by `radius`.
//  * Contact normals point from shape A to shape B; `depth` >= 0 is the penetration along the normal. Shapes that
//    exactly touch (separation == 0) produce a contact with depth 0; any positive separation produces none.
//  * A ray that starts inside a shape (or on its surface) hits it at distance 0 with normal -direction.
//
// Thread safety: every function here is pure and may be called concurrently.
#pragma once

#include "titan/math.h"

#include <optional>

namespace titan::physics {

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;

    static Quat axisAngle(const Vec3& unitAxis, float radians);
    static Quat fromEulerDegrees(const Vec3& degrees);  // same rotation as Transform::rotation
    Quat operator*(const Quat& o) const;                 // Hamilton product: apply o, then *this
    Quat conjugate() const { return {-x, -y, -z, w}; }
    float norm() const;
    Quat normalized() const;  // identity when the norm is zero or not finite
    Vec3 rotate(const Vec3& v) const;
    bool finite() const;
    bool operator==(const Quat& o) const { return x == o.x && y == o.y && z == o.z && w == o.w; }
};

enum class ShapeKind { Sphere, Box, Capsule };

struct Shape {
    ShapeKind kind = ShapeKind::Sphere;
    float radius = 0.5f;               // sphere, capsule
    Vec3 halfExtents{0.5f, 0.5f, 0.5f};  // box
    float halfHeight = 0.5f;           // capsule: half-length of the inner segment (may be 0: a sphere)

    static Shape sphere(float r) { Shape s; s.kind = ShapeKind::Sphere; s.radius = r; return s; }
    static Shape box(const Vec3& h) { Shape s; s.kind = ShapeKind::Box; s.halfExtents = h; return s; }
    static Shape capsule(float r, float hh) { Shape s; s.kind = ShapeKind::Capsule; s.radius = r; s.halfHeight = hh; return s; }
    bool operator==(const Shape& o) const;
};

// A shape placed in the world.
struct Placed {
    Shape shape;
    Vec3 position;
    Quat rotation;
};

struct Aabb {
    Vec3 min, max;
    bool overlaps(const Aabb& o) const {
        return min.x <= o.max.x && o.min.x <= max.x && min.y <= o.max.y && o.min.y <= max.y && min.z <= o.max.z &&
               o.min.z <= max.z;
    }
};

struct ContactPoint {
    Vec3 normal;  // unit, from A to B
    float depth = 0;
    Vec3 point;   // representative world-space point between the surfaces
};

struct RayHit {
    float distance = 0;
    Vec3 point;
    Vec3 normal;
};

bool finite(const Vec3& v);
Aabb worldAabb(const Placed& p);

// Closest point on segment [a, b] to p; `t` (optional) receives the segment parameter in [0, 1].
Vec3 closestPointOnSegment(const Vec3& p, const Vec3& a, const Vec3& b, float* t = nullptr);
// Closest points between segments [p1, q1] and [p2, q2] (handles degenerate and parallel segments).
void closestPointsSegments(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2, Vec3& c1, Vec3& c2);
// Closest point of an oriented box (centre, rotation, half extents) to p; p itself when inside.
Vec3 closestPointOnBox(const Vec3& p, const Vec3& centre, const Quat& rotation, const Vec3& halfExtents);

// Contact between two placed shapes, or nullopt when they are separated.
std::optional<ContactPoint> collide(const Placed& a, const Placed& b);
// First hit along origin + t * unitDirection for t in [0, maxDistance], or nullopt.
std::optional<RayHit> raycast(const Placed& p, const Vec3& origin, const Vec3& unitDirection, float maxDistance);

}  // namespace titan::physics
