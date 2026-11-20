#include "TitanVisibility.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace Titan {
namespace {
V3 Sub(V3 a, V3 b) { return {a.X - b.X, a.Y - b.Y, a.Z - b.Z}; }
double Dot(V3 a, V3 b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
V3 Cross(V3 a, V3 b) { return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X}; }
double Length(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Scale(V3 a, double s) { return {a.X * s, a.Y * s, a.Z * s}; }
V3 Normalize(V3 a) {
    const double len = Length(a);
    if (!std::isfinite(len) || len < 1e-12) throw std::invalid_argument("camera basis vector invalid");
    return Scale(a, 1.0 / len);
}
bool Finite(V3 v) { return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z); }
}
DrawList BuildDrawList(const Camera& c, const std::vector<RenderObject>& objects,
                       double high, double mid) {
    if (!Finite(c.Position) || !Finite(c.Forward) || !Finite(c.Up) ||
        !std::isfinite(c.VerticalFovRadians) || c.VerticalFovRadians <= 0 || c.VerticalFovRadians >= 3.13 ||
        !std::isfinite(c.Aspect) || c.Aspect <= 0 || !std::isfinite(c.Near) || c.Near <= 0 ||
        !std::isfinite(c.Far) || c.Far <= c.Near || c.ScreenHeightPixels <= 0 ||
        !std::isfinite(high) || !std::isfinite(mid) || high <= mid || mid < 0)
        throw std::invalid_argument("invalid view / LOD configuration");
    const V3 forward = Normalize(c.Forward);
    // Orthogonalize camera up: handles non-perpendicular valid input vectors.
    const V3 right = Normalize(Cross(c.Up, forward));
    const V3 up = Cross(forward, right);
    const double tanY = std::tan(c.VerticalFovRadians / 2.0);
    const double tanX = tanY * c.Aspect;
    const double sideMarginX = std::sqrt(1.0 + tanX * tanX);
    const double sideMarginY = std::sqrt(1.0 + tanY * tanY);
    DrawList result;
    result.Items.reserve(objects.size());
    for (const RenderObject& item : objects) {
        if (!Finite(item.Center) || !std::isfinite(item.Radius) || item.Radius < 0) {
            ++result.Invalid; continue;
        }
        const V3 offset = Sub(item.Center, c.Position);
        const double z = Dot(offset, forward);
        const double x = Dot(offset, right);
        const double y = Dot(offset, up);
        if (z + item.Radius < c.Near || z - item.Radius > c.Far ||
            std::abs(x) > z * tanX + item.Radius * sideMarginX ||
            std::abs(y) > z * tanY + item.Radius * sideMarginY) {
            ++result.Culled; continue;
        }
        const double distance = Length(offset);
        const double projectedRadius = item.Radius * c.ScreenHeightPixels / (2.0 * tanY * std::max(z, c.Near));
        const int lod = projectedRadius >= high ? 0 : projectedRadius >= mid ? 1 : 2;
        result.Items.push_back({item.Id, item.Mesh, item.Material, lod, distance});
    }
    std::sort(result.Items.begin(), result.Items.end(), [](const DrawItem& a, const DrawItem& b) {
        return std::tie(a.Mesh, a.Material, a.Lod, a.Id) < std::tie(b.Mesh, b.Material, b.Lod, b.Id);
    });
    for (std::size_t i = 0; i < result.Items.size(); ++i) {
        const DrawItem& item = result.Items[i];
        if (result.Batches.empty() || result.Batches.back().Mesh != item.Mesh ||
            result.Batches.back().Material != item.Material || result.Batches.back().Lod != item.Lod) {
            result.Batches.push_back({item.Mesh, item.Material, item.Lod, i, 1});
        } else {
            ++result.Batches.back().Count;
        }
    }
    return result;
}
} // namespace Titan
