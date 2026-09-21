#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/lighting/LightingView.hpp>
#include <orbit/math/Vector.hpp>

#include <span>
#include <vector>

namespace orbit::lighting
{
struct EmissiveSurfaceGrid
{
    frames::FrameId frame{};
    universe::BodyId body{};
    u64 stableId{0U};

    // Rectangle authority in frame coordinates. origin is the lower-left
    // corner; axisU/axisV span the complete physical emitting surface.
    math::Double3 originInFrameMeters{};
    math::Double3 axisUInFrameMeters{1.0, 0.0, 0.0};
    math::Double3 axisVInFrameMeters{0.0, 1.0, 0.0};

    u32 width{0U};
    u32 height{0U};

    // Scene-linear GI radiance, row-major, width*height samples.
    std::span<const math::Float3> giRadiance;
};

struct EmissiveHierarchyNode
{
    u32 firstChild{~0U};
    u32 childCount{0U};

    u32 texelMinX{0U};
    u32 texelMinY{0U};
    u32 texelMaxX{0U};
    u32 texelMaxY{0U};

    math::Double3 centerInFrameMeters{};
    math::Float3 averageRadiance{};
    math::Float3 peakRadiance{};

    f64 areaMetersSquared{0.0};
    f64 radiantImportance{0.0};

    u32 level{0U};

    [[nodiscard]] bool IsLeaf() const noexcept
    {
        return childCount == 0U;
    }
};

struct EmissiveHierarchy
{
    frames::FrameId frame{};
    universe::BodyId body{};
    u64 stableId{0U};

    math::Double3 surfaceNormal{};
    std::vector<EmissiveHierarchyNode> nodes;
    u32 root{~0U};
    u32 sourceWidth{0U};
    u32 sourceHeight{0U};
};

struct EmissiveHierarchySelectionConfig
{
    // Descend while a node contributes at least this many projected pixels.
    f32 subdivisionProjectedPixels{24.0F};

    // Tiny but extremely energetic nodes may still be refined.
    f64 minimumRadiantImportance{0.0};

    u32 maximumSelectedNodes{4096U};
};

struct EmissiveSelectedNode
{
    u32 nodeIndex{0U};
    f32 projectedPixels{0.0F};
    f64 weightedImportance{0.0};
};

[[nodiscard]] EmissiveHierarchy
BuildEmissiveHierarchy(
    const EmissiveSurfaceGrid& surface);

[[nodiscard]] std::vector<EmissiveSelectedNode>
SelectEmissiveHierarchy(
    const EmissiveHierarchy& hierarchy,
    const LightingView& view,
    u32 viewportWidth,
    u32 viewportHeight,
    const EmissiveHierarchySelectionConfig& config = {});
} // namespace orbit::lighting
