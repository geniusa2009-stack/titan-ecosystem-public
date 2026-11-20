// TITAN Engine — procedural primitive meshes matching the picking shapes in scene.h
// (all primitives fit the unit box [-0.5, 0.5]^3; the plane lies at y = 0).
// Original TITAN code.
#pragma once

#include "titan/scene.h"

#include <cstdint>
#include <vector>

namespace titan {

struct MeshData {
    // Interleaved position.xyz, normal.xyz
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    size_t vertexCount() const { return vertices.size() / 6; }
};

MeshData makeMesh(Shape shape, int segments = 32);
// Line-list geometry (positions + dummy normals) for the editor grid and selection box.
MeshData makeGridLines(int halfExtent, float spacing);
MeshData makeUnitBoxLines();

}  // namespace titan
