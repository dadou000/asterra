#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/lighting/LightingView.hpp>
#include <orbit/math/Vector.hpp>

#include <array>
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
    std::array<u32, 4> children{
        ~0U, ~0U, ~0U, ~0U};
    u32 childCount{0U};

    u32 texelMinX{0U};
    u32 texelMinY{0U};
    u32 texelMaxX{0U};
    u32 texelMaxY{0U};

    math::Double3 centerInFrameMeters{};
    math::Float3 averageRadiance{};
    math::Float3 peakRadiance{};

    // Integral of GI radiance over physical emitting area. This is the
    // energy-preserving quantity used when a subtree collapses to one node.
    math::Float3 integratedRadianceArea{};

    // Brightest luminance represented by this node; used by M17 to promote
    // tiny high-energy emitters that projected-area selection would miss.
    f64 peakLuminance{0.0};

    // Source-space centroid weighted by luminance * physical texel area.
    // This remains stable as the hierarchy collapses/refines.
    math::Double2 energyWeightedUv{0.5, 0.5};

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

struct EmissiveHierarchyBuildConfig
{
    // A 4K surface with the default 8x8 leaves has ~130k leaf tiles rather
    // than one runtime object/node per pixel. Near-field color detail is
    // preserved at tile resolution; source appearance remains full-res.
    u32 leafTileWidth{8U};
    u32 leafTileHeight{8U};
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
    const EmissiveSurfaceGrid& surface,
    const EmissiveHierarchyBuildConfig& config = {});

[[nodiscard]] std::vector<EmissiveSelectedNode>
SelectEmissiveHierarchy(
    const EmissiveHierarchy& hierarchy,
    const LightingView& view,
    u32 viewportWidth,
    u32 viewportHeight,
    const EmissiveHierarchySelectionConfig& config = {});
} // namespace orbit::lighting
