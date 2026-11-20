// TITAN Engine — small value-type math used by the scene model, picking and the Studio
// renderer, so that the engine and the editor agree bit-for-bit on transforms.
// Original TITAN code.
//
// Conventions (documented in docs/ARCHITECTURE.md):
//  * Right-handed, Y-up, metres.
//  * Rotation is Euler degrees (pitch=X, yaw=Y, roll=Z) composed as R = Ry * Rx * Rz.
//  * Mat4 is column-major: m[col * 4 + row].
#pragma once

#include <array>
#include <cmath>
#include <optional>

namespace titan {

constexpr float kPi = 3.14159265358979323846f;
inline float radians(float deg) { return deg * (kPi / 180.0f); }

struct Vec3 {
    float x = 0, y = 0, z = 0;
    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    // Rounds x and z to multiples of `step` (ground-plane snapping); y and non-positive steps are unchanged.
    Vec3 snappedXZ(float step) const {
        if (!(step > 0)) return *this;
        return {std::round(x / step) * step, y, std::round(z / step) * step};
    }
    bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Vec3& o) const { return !(*this == o); }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
    float l = length(v);
    return l > 0 ? v * (1.0f / l) : Vec3{};
}

struct Mat4 {
    std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    float& at(int row, int col) { return m[static_cast<size_t>(col * 4 + row)]; }
    float at(int row, int col) const { return m[static_cast<size_t>(col * 4 + row)]; }

    static Mat4 identity() { return {}; }
    static Mat4 translation(const Vec3& t);
    static Mat4 scaling(const Vec3& s);
    static Mat4 rotationX(float deg);
    static Mat4 rotationY(float deg);
    static Mat4 rotationZ(float deg);
    static Mat4 perspective(float fovYDeg, float aspect, float zNear, float zFar);
    static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up);

    Mat4 operator*(const Mat4& o) const;
    Vec3 transformPoint(const Vec3& p) const;
    Vec3 transformDirection(const Vec3& d) const;
    std::optional<Mat4> inverse() const;
};

struct Transform {
    Vec3 position{0, 0, 0};
    Vec3 rotation{0, 0, 0};  // Euler degrees, see header comment for order
    Vec3 scale{1, 1, 1};

    Mat4 matrix() const;
    // Inverse of matrix(): the translation/rotation/scale that reproduce `m`, or nullopt when `m` cannot be
    // written as T*R*S (shear from a non-uniformly scaled, rotated parent, or a singular matrix). Values within
    // 1e-4 of an integer are snapped so round trips stay readable in the Inspector.
    static std::optional<Transform> fromMatrix(const Mat4& m);
    bool operator==(const Transform& o) const {
        return position == o.position && rotation == o.rotation && scale == o.scale;
    }
    bool operator!=(const Transform& o) const { return !(*this == o); }
};

struct Ray {
    Vec3 origin;
    Vec3 direction;  // need not be normalized
};

}  // namespace titan
