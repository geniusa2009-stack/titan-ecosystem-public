// TITAN Engine — primitive mesh generation. Original TITAN code.
#include "titan/mesh.h"

#include <cmath>

namespace titan {

namespace {
void vtx(MeshData& m, Vec3 p, Vec3 n) {
    m.vertices.insert(m.vertices.end(), {p.x, p.y, p.z, n.x, n.y, n.z});
}
uint32_t base(const MeshData& m) { return static_cast<uint32_t>(m.vertexCount()); }

void quad(MeshData& m, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n) {  // counter-clockwise from outside
    uint32_t i = base(m);
    vtx(m, a, n); vtx(m, b, n); vtx(m, c, n); vtx(m, d, n);
    m.indices.insert(m.indices.end(), {i, i + 1, i + 2, i, i + 2, i + 3});
}

MeshData cube() {
    MeshData m;
    const float h = 0.5f;
    quad(m, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, {0, 0, 1});      // +Z
    quad(m, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {0, 0, -1});  // -Z
    quad(m, {h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}, {1, 0, 0});      // +X
    quad(m, {-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-1, 0, 0});  // -X
    quad(m, {-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}, {0, 1, 0});      // +Y
    quad(m, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}, {0, -1, 0});  // -Y
    return m;
}

MeshData plane() {
    MeshData m;
    quad(m, {-0.5f, 0, 0.5f}, {0.5f, 0, 0.5f}, {0.5f, 0, -0.5f}, {-0.5f, 0, -0.5f}, {0, 1, 0});
    return m;
}

MeshData sphere(int seg) {
    MeshData m;
    const int rings = seg / 2;
    for (int r = 0; r <= rings; ++r) {
        float v = float(r) / rings, phi = v * kPi;
        for (int s = 0; s <= seg; ++s) {
            float u = float(s) / seg, theta = u * 2 * kPi;
            Vec3 n{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            vtx(m, n * 0.5f, n);
        }
    }
    const uint32_t stride = uint32_t(seg + 1);
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < seg; ++s) {
            uint32_t a = uint32_t(r) * stride + uint32_t(s), b = a + stride;
            m.indices.insert(m.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    return m;
}

MeshData cylinder(int seg) {
    MeshData m;
    for (int s = 0; s <= seg; ++s) {  // side
        float t = float(s) / seg * 2 * kPi;
        Vec3 n{std::cos(t), 0, std::sin(t)};
        vtx(m, {n.x * 0.5f, -0.5f, n.z * 0.5f}, n);
        vtx(m, {n.x * 0.5f, 0.5f, n.z * 0.5f}, n);
    }
    for (int s = 0; s < seg; ++s) {
        uint32_t a = uint32_t(s) * 2;
        m.indices.insert(m.indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
    }
    for (float y : {0.5f, -0.5f}) {  // caps
        Vec3 n{0, y > 0 ? 1.0f : -1.0f, 0};
        uint32_t c = base(m);
        vtx(m, {0, y, 0}, n);
        for (int s = 0; s <= seg; ++s) {
            float t = float(s) / seg * 2 * kPi;
            vtx(m, {std::cos(t) * 0.5f, y, std::sin(t) * 0.5f}, n);
        }
        for (int s = 0; s < seg; ++s) {
            uint32_t a = c + 1 + uint32_t(s);
            if (y > 0) m.indices.insert(m.indices.end(), {c, a + 1, a});
            else m.indices.insert(m.indices.end(), {c, a, a + 1});
        }
    }
    return m;
}
}  // namespace

MeshData makeMesh(Shape shape, int segments) {
    switch (shape) {
        case Shape::Cube: return cube();
        case Shape::Plane: return plane();
        case Shape::Sphere: return sphere(segments);
        case Shape::Cylinder: return cylinder(segments);
        case Shape::None: break;
    }
    return {};
}

MeshData makeGridLines(int halfExtent, float spacing) {
    MeshData m;
    const float e = halfExtent * spacing;
    for (int i = -halfExtent; i <= halfExtent; ++i) {
        float p = i * spacing;
        uint32_t b = base(m);
        vtx(m, {p, 0, -e}, {0, 1, 0});
        vtx(m, {p, 0, e}, {0, 1, 0});
        vtx(m, {-e, 0, p}, {0, 1, 0});
        vtx(m, {e, 0, p}, {0, 1, 0});
        m.indices.insert(m.indices.end(), {b, b + 1, b + 2, b + 3});
    }
    return m;
}

MeshData makeUnitBoxLines() {
    MeshData m;
    const float h = 0.5f;
    for (int i = 0; i < 8; ++i)
        vtx(m, {(i & 1) ? h : -h, (i & 2) ? h : -h, (i & 4) ? h : -h}, {0, 1, 0});
    m.indices = {0, 1, 2, 3, 4, 5, 6, 7, 0, 2, 1, 3, 4, 6, 5, 7, 0, 4, 1, 5, 2, 6, 3, 7};
    return m;
}

}  // namespace titan
