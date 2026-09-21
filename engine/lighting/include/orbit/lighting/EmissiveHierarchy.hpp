#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <span>
#include <vector>

namespace orbit::lighting
{
struct EmissiveHierarchySampleGrid
{
    u32 width{0U};
    u32 height{0U};
    f32 texelAreaSquareMeters{0.0F};
    std::span<const math::Float3> giRadiance;
};

struct EmissiveHierarchyNode
{
    u32 level{0U};
    u32 x{0U};
    u32 y{0U};
    u32 widthTexels{0U};
    u32 heightTexels{0U};

    math::Float3 integratedEnergy{};
    math::Float2 centroidUv{0.5F, 0.5F};
    f32 totalAreaSquareMeters{0.0F};
    f32 peakLuminance{0.0F};

    u32 firstChild{~0U};
    u32 childCount{0U};

    [[nodiscard]] bool IsLeaf() const noexcept
    {
        return childCount == 0U;
    }
};

struct EmissiveHierarchy
{
    u32 sourceWidth{0U};
    u32 sourceHeight{0U};
    f32 texelAreaSquareMeters{0.0F};

    std::vector<EmissiveHierarchyNode> nodes;
    u32 root{~0U};
};

struct EmissiveHierarchySelectionSettings
{
    // Nodes larger than this on screen are refined when children are
    // available. Far surfaces therefore collapse automatically.
    f32 refineAboveProjectedPixels{24.0F};

    // Hard scheduler cap. Selection remains energy-aware when the cap is hit.
    u32 maximumNodes{256U};
};

struct SelectedEmissiveNode
{
    u32 nodeIndex{0U};
    math::Float3 integratedEnergy{};
    math::Float2 centroidUv{};
    f32 projectedWidthPixels{0.0F};
    f32 importance{0.0F};
};

[[nodiscard]] EmissiveHierarchy BuildEmissiveHierarchy(
    const EmissiveHierarchySampleGrid& source);

[[nodiscard]] std::vector<SelectedEmissiveNode>
SelectEmissiveHierarchyNodes(
    const EmissiveHierarchy& hierarchy,
    f32 projectedSurfaceWidthPixels,
    const EmissiveHierarchySelectionSettings& settings = {});

[[nodiscard]] u32 EmissiveHierarchyLeafCount(
    const EmissiveHierarchy& hierarchy) noexcept;
} // namespace orbit::lighting
