#pragma once

#include <orbit/core/Types.hpp>

#include <string>
#include <string_view>
#include <vector>

// Wavefront OBJ parsing for the Shading tab's mesh preview shape. CPU only:
// the renderer uploads the result, so this is testable without a device.
namespace orbit::shading
{
// Layout matches the preview vertex format (32 bytes).
struct MeshVertex
{
    f32 position[3]{};
    f32 normal[3]{};
    f32 uv[2]{};
};

struct MeshData
{
    std::vector<MeshVertex> vertices;
    std::vector<u32> indices;

    // Extent of the source geometry before it was fitted to the preview.
    f32 sourceMin[3]{};
    f32 sourceMax[3]{};
    // Radius of the source geometry about its bounding-box centre.
    f32 sourceRadius{0.0F};

    bool hadNormals{false};
    bool hadUvs{false};
    u32 sourceFaces{0U};

    [[nodiscard]] u32 TriangleCount() const noexcept
    {
        return static_cast<u32>(indices.size() / 3U);
    }
};

// Parses `v`, `vt`, `vn` and `f` records (positive or negative indices; `f`
// with 3 or more vertices is triangulated as a fan, so polygons must be
// convex). Everything else (`o`, `g`, `s`, `usemtl`, `mtllib`, comments) is
// ignored.
//
// The result is fitted to the preview: translated so its bounding box is
// centred on the origin and uniformly scaled so it fits a unit sphere, which
// puts any model at a sensible size next to the sphere/cube/plane shapes.
// Missing normals become smooth (area-weighted) normals shared by position, so
// UV seams do not show as lighting seams; missing UVs become a spherical
// projection.
//
// Throws std::runtime_error with a "line N: ..." message for malformed input,
// an out-of-range index, non-finite numbers, or a file with no faces.
[[nodiscard]] MeshData ParseObj(std::string_view text);
} // namespace orbit::shading
