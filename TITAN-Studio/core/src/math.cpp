// TITAN Engine — math implementation. Original TITAN code.
#include "titan/math.h"

#include <algorithm>

namespace titan {

Mat4 Mat4::translation(const Vec3& t) {
    Mat4 r;
    r.at(0, 3) = t.x;
    r.at(1, 3) = t.y;
    r.at(2, 3) = t.z;
    return r;
}

Mat4 Mat4::scaling(const Vec3& s) {
    Mat4 r;
    r.at(0, 0) = s.x;
    r.at(1, 1) = s.y;
    r.at(2, 2) = s.z;
    return r;
}

Mat4 Mat4::rotationX(float deg) {
    float c = std::cos(radians(deg)), s = std::sin(radians(deg));
    Mat4 r;
    r.at(1, 1) = c;
    r.at(1, 2) = -s;
    r.at(2, 1) = s;
    r.at(2, 2) = c;
    return r;
}

Mat4 Mat4::rotationY(float deg) {
    float c = std::cos(radians(deg)), s = std::sin(radians(deg));
    Mat4 r;
    r.at(0, 0) = c;
    r.at(0, 2) = s;
    r.at(2, 0) = -s;
    r.at(2, 2) = c;
    return r;
}

Mat4 Mat4::rotationZ(float deg) {
    float c = std::cos(radians(deg)), s = std::sin(radians(deg));
    Mat4 r;
    r.at(0, 0) = c;
    r.at(0, 1) = -s;
    r.at(1, 0) = s;
    r.at(1, 1) = c;
    return r;
}

Mat4 Mat4::perspective(float fovYDeg, float aspect, float zNear, float zFar) {
    // OpenGL-style clip space (z in [-1, 1]); the Studio renderer applies
    // QRhi::clipSpaceCorrMatrix() to adapt this to Metal/Vulkan/D3D.
    float f = 1.0f / std::tan(radians(fovYDeg) * 0.5f);
    Mat4 r;
    r.m.fill(0);
    r.at(0, 0) = f / aspect;
    r.at(1, 1) = f;
    r.at(2, 2) = (zFar + zNear) / (zNear - zFar);
    r.at(2, 3) = (2.0f * zFar * zNear) / (zNear - zFar);
    r.at(3, 2) = -1.0f;
    return r;
}

Mat4 Mat4::lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
    Vec3 f = normalize(target - eye);
    Vec3 s = normalize(cross(f, up));
    Vec3 u = cross(s, f);
    Mat4 r;
    r.at(0, 0) = s.x; r.at(0, 1) = s.y; r.at(0, 2) = s.z;
    r.at(1, 0) = u.x; r.at(1, 1) = u.y; r.at(1, 2) = u.z;
    r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z;
    r.at(0, 3) = -dot(s, eye);
    r.at(1, 3) = -dot(u, eye);
    r.at(2, 3) = dot(f, eye);
    return r;
}

Mat4 Mat4::operator*(const Mat4& o) const {
    Mat4 r;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) sum += at(row, k) * o.at(k, col);
            r.at(row, col) = sum;
        }
    return r;
}

Vec3 Mat4::transformPoint(const Vec3& p) const {
    float x = at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z + at(0, 3);
    float y = at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z + at(1, 3);
    float z = at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z + at(2, 3);
    float w = at(3, 0) * p.x + at(3, 1) * p.y + at(3, 2) * p.z + at(3, 3);
    if (w != 0.0f && w != 1.0f) return {x / w, y / w, z / w};
    return {x, y, z};
}

Vec3 Mat4::transformDirection(const Vec3& d) const {
    return {at(0, 0) * d.x + at(0, 1) * d.y + at(0, 2) * d.z,
            at(1, 0) * d.x + at(1, 1) * d.y + at(1, 2) * d.z,
            at(2, 0) * d.x + at(2, 1) * d.y + at(2, 2) * d.z};
}

std::optional<Mat4> Mat4::inverse() const {
    // Cofactor expansion on the column-major array (standard formulation).
    const auto& a = m;
    std::array<float, 16> inv{};
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] +
             a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] -
             a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
             a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
              a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
             a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
             a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
             a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] +
              a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
             a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
             a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
              a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] -
              a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] -
             a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
             a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
              a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] +
              a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (std::fabs(det) < 1e-12f) return std::nullopt;
    Mat4 r;
    for (size_t i = 0; i < 16; ++i) r.m[i] = inv[i] / det;
    return r;
}

Mat4 Transform::matrix() const {
    return Mat4::translation(position) * Mat4::rotationY(rotation.y) * Mat4::rotationX(rotation.x) *
           Mat4::rotationZ(rotation.z) * Mat4::scaling(scale);
}

std::optional<Transform> Transform::fromMatrix(const Mat4& m) {
    Vec3 c[3];
    for (int k = 0; k < 3; ++k) c[k] = {m.at(0, k), m.at(1, k), m.at(2, k)};
    Vec3 s{length(c[0]), length(c[1]), length(c[2])};
    if (!(s.x > 1e-6f && s.y > 1e-6f && s.z > 1e-6f)) return std::nullopt;
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b)
            if (std::fabs(dot(c[a], c[b])) > 1e-4f * length(c[a]) * length(c[b])) return std::nullopt;  // shear
    if (dot(cross(c[0], c[1]), c[2]) < 0) s.x = -s.x;  // mirrored: keep R a proper rotation
    const float sv[3] = {s.x, s.y, s.z};
    auto r = [&](int row, int col) { return (row == 0 ? c[col].x : row == 1 ? c[col].y : c[col].z) / sv[col]; };
    // R = Ry * Rx * Rz  =>  r12 = -sin(x), r02/r22 = tan(y), r10/r11 = tan(z).
    const float deg = 180.0f / kPi;
    Transform t;
    t.position = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    t.scale = s;
    const float sx = std::clamp(-r(1, 2), -1.0f, 1.0f);
    t.rotation.x = std::asin(sx) * deg;
    if (std::fabs(sx) < 0.99999f) {
        t.rotation.y = std::atan2(r(0, 2), r(2, 2)) * deg;
        t.rotation.z = std::atan2(r(1, 0), r(1, 1)) * deg;
    } else {  // gimbal lock: fold roll into yaw
        t.rotation.y = std::atan2(-r(2, 0), r(0, 0)) * deg;
        t.rotation.z = 0;
    }
    auto snap = [](float& v) {
        const float n = std::round(v);
        if (std::fabs(v - n) < 1e-4f) v = n == 0 ? 0.0f : n;
    };
    for (float* v : {&t.position.x, &t.position.y, &t.position.z, &t.rotation.x, &t.rotation.y, &t.rotation.z,
                     &t.scale.x, &t.scale.y, &t.scale.z})
        snap(*v);
    return t;
}

}  // namespace titan
