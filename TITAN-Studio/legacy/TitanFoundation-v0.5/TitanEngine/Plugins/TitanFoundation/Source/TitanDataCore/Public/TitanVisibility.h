#pragma once
// View-frustum sphere culling, screen-size LOD, material/mesh draw batching.
// This is a CPU-side draw-list builder, NOT a GPU renderer or occlusion-culling system.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Titan {
struct V3 { double X = 0, Y = 0, Z = 0; };
struct Camera {
    V3 Position;
    V3 Forward{0, 0, 1};
    V3 Up{0, 1, 0};
    double VerticalFovRadians = 1.0471975511965976;
    double Aspect = 16.0 / 9.0;
    double Near = 0.1;
    double Far = 1000.0;
    int ScreenHeightPixels = 1080;
};
struct RenderObject {
    uint64_t Id = 0;
    V3 Center;
    double Radius = 1.0;
    uint32_t Mesh = 0;
    uint32_t Material = 0;
};
struct DrawItem { uint64_t Id; uint32_t Mesh, Material; int Lod; double Distance; };
struct DrawBatch { uint32_t Mesh, Material; int Lod; std::size_t Offset, Count; };
struct DrawList {
    std::vector<DrawItem> Items;
    std::vector<DrawBatch> Batches;
    std::size_t Culled = 0;
    std::size_t Invalid = 0;
};
// lod0MinPixels and lod1MinPixels define projected sphere radius thresholds.
DrawList BuildDrawList(const Camera& camera, const std::vector<RenderObject>& objects,
                       double lod0MinPixels = 80.0, double lod1MinPixels = 20.0);
} // namespace Titan
