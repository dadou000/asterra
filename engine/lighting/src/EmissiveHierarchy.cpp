#include <orbit/lighting/EmissiveHierarchy.hpp>

#include <algorithm>
#include <cmath>
#include <queue>
#include <stdexcept>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] f32 Luminance(
    const math::Float3 value) noexcept
{
    return
        std::max(value.x, 0.0F) * 0.2126F +
        std::max(value.y, 0.0F) * 0.7152F +
        std::max(value.z, 0.0F) * 0.0722F;
}

struct BuildContext
{
    const EmissiveHierarchySampleGrid* source{nullptr};
    EmissiveHierarchy* hierarchy{nullptr};
};

u32 BuildNode(
    BuildContext& context,
    const u32 x,
    const u32 y,
    const u32 width,
    const u32 height,
    const u32 level)
{
    const u32 nodeIndex =
        static_cast<u32>(
            context.hierarchy->nodes.size());

    context.hierarchy->nodes.push_back({
        .level = level,
        .x = x,
        .y = y,
        .widthTexels = width,
        .heightTexels = height
    });

    auto& node =
        context.hierarchy->nodes[nodeIndex];

    if (width == 1U &&
        height == 1U)
    {
        const auto radiance =
            context.source->giRadiance[
                static_cast<std::size_t>(y) *
                    context.source->width +
                x];

        node.totalAreaSquareMeters =
            context.source->texelAreaSquareMeters;

        node.integratedEnergy = {
            std::max(radiance.x, 0.0F) *
                node.totalAreaSquareMeters,
            std::max(radiance.y, 0.0F) *
                node.totalAreaSquareMeters,
            std::max(radiance.z, 0.0F) *
                node.totalAreaSquareMeters
        };

        node.peakLuminance =
            Luminance(radiance);

        node.centroidUv = {
            (static_cast<f32>(x) + 0.5F) /
                static_cast<f32>(
                    context.source->width),
            (static_cast<f32>(y) + 0.5F) /
                static_cast<f32>(
                    context.source->height)
        };

        return nodeIndex;
    }

    const u32 leftWidth =
        (width + 1U) / 2U;
    const u32 rightWidth =
        width - leftWidth;
    const u32 topHeight =
        (height + 1U) / 2U;
    const u32 bottomHeight =
        height - topHeight;

    struct Region
    {
        u32 x;
        u32 y;
        u32 width;
        u32 height;
    };

    const Region regions[4]{
        {x, y, leftWidth, topHeight},
        {x + leftWidth, y, rightWidth, topHeight},
        {x, y + topHeight, leftWidth, bottomHeight},
        {x + leftWidth, y + topHeight, rightWidth, bottomHeight}
    };

    const u32 firstChild =
        static_cast<u32>(
            context.hierarchy->nodes.size());

    u32 childCount = 0U;

    math::Float3 integrated{};
    f32 area = 0.0F;
    f32 peak = 0.0F;
    f32 energyWeight = 0.0F;
    math::Float2 weightedCentroid{};

    for (const auto& region : regions)
    {
        if (region.width == 0U ||
            region.height == 0U)
        {
            continue;
        }

        const u32 childIndex =
            BuildNode(
                context,
                region.x,
                region.y,
                region.width,
                region.height,
                level + 1U);

        ++childCount;

        const auto& child =
            context.hierarchy->
                nodes[childIndex];

        integrated.x +=
            child.integratedEnergy.x;
        integrated.y +=
            child.integratedEnergy.y;
        integrated.z +=
            child.integratedEnergy.z;

        area +=
            child.totalAreaSquareMeters;
        peak =
            std::max(
                peak,
                child.peakLuminance);

        const f32 childEnergy =
            std::max(
                Luminance(
                    child.integratedEnergy),
                0.0F);

        weightedCentroid.x +=
            child.centroidUv.x *
            childEnergy;
        weightedCentroid.y +=
            child.centroidUv.y *
            childEnergy;
        energyWeight +=
            childEnergy;
    }

    node = {
        .level = level,
        .x = x,
        .y = y,
        .widthTexels = width,
        .heightTexels = height,
        .integratedEnergy = integrated,
        .centroidUv =
            energyWeight > 1.0e-8F
                ? math::Float2{
                      weightedCentroid.x /
                          energyWeight,
                      weightedCentroid.y /
                          energyWeight}
                : math::Float2{
                      (static_cast<f32>(x) +
                       static_cast<f32>(width) *
                           0.5F) /
                          static_cast<f32>(
                              context.source->width),
                      (static_cast<f32>(y) +
                       static_cast<f32>(height) *
                           0.5F) /
                          static_cast<f32>(
                              context.source->height)},
        .totalAreaSquareMeters = area,
        .peakLuminance = peak,
        .firstChild = firstChild,
        .childCount = childCount
    };

    return nodeIndex;
}

struct Candidate
{
    u32 nodeIndex{0U};
    f32 projectedPixels{0.0F};
    f32 importance{0.0F};

    bool operator<(
        const Candidate& other) const noexcept
    {
        return importance <
            other.importance;
    }
};

[[nodiscard]] f32 NodeProjectedPixels(
    const EmissiveHierarchy& hierarchy,
    const EmissiveHierarchyNode& node,
    const f32 surfacePixels) noexcept
{
    if (hierarchy.sourceWidth == 0U)
    {
        return 0.0F;
    }

    return
        surfacePixels *
        static_cast<f32>(
            node.widthTexels) /
        static_cast<f32>(
            hierarchy.sourceWidth);
}

[[nodiscard]] f32 NodeImportance(
    const EmissiveHierarchyNode& node,
    const f32 projectedPixels) noexcept
{
    return
        Luminance(
            node.integratedEnergy) *
        std::max(
            projectedPixels,
            0.0F);
}
} // namespace

EmissiveHierarchy BuildEmissiveHierarchy(
    const EmissiveHierarchySampleGrid& source)
{
    if (source.width == 0U ||
        source.height == 0U ||
        !std::isfinite(
            source.texelAreaSquareMeters) ||
        source.texelAreaSquareMeters <= 0.0F ||
        source.giRadiance.size() !=
            static_cast<std::size_t>(
                source.width) *
            source.height)
    {
        throw std::invalid_argument(
            "Emissive hierarchy source grid is invalid.");
    }

    EmissiveHierarchy result{
        .sourceWidth = source.width,
        .sourceHeight = source.height,
        .texelAreaSquareMeters =
            source.texelAreaSquareMeters
    };

    result.nodes.reserve(
        static_cast<std::size_t>(
            source.width) *
        source.height *
        2U);

    BuildContext context{
        .source = &source,
        .hierarchy = &result
    };

    result.root =
        BuildNode(
            context,
            0U,
            0U,
            source.width,
            source.height,
            0U);

    return result;
}

std::vector<SelectedEmissiveNode>
SelectEmissiveHierarchyNodes(
    const EmissiveHierarchy& hierarchy,
    const f32 projectedSurfaceWidthPixels,
    const EmissiveHierarchySelectionSettings& settings)
{
    std::vector<SelectedEmissiveNode>
        result;

    if (hierarchy.nodes.empty() ||
        hierarchy.root >=
            hierarchy.nodes.size() ||
        settings.maximumNodes == 0U)
    {
        return result;
    }

    std::priority_queue<Candidate>
        pending;

    const auto pushNode =
        [&](const u32 index)
        {
            const auto& node =
                hierarchy.nodes[index];

            const f32 projected =
                NodeProjectedPixels(
                    hierarchy,
                    node,
                    projectedSurfaceWidthPixels);

            pending.push({
                .nodeIndex = index,
                .projectedPixels =
                    projected,
                .importance =
                    NodeImportance(
                        node,
                        projected)
            });
        };

    pushNode(
        hierarchy.root);

    while (!pending.empty())
    {
        const Candidate candidate =
            pending.top();
        pending.pop();

        const auto& node =
            hierarchy.nodes[
                candidate.nodeIndex];

        const bool shouldRefine =
            !node.IsLeaf() &&
            candidate.projectedPixels >
                settings.
                    refineAboveProjectedPixels &&
            result.size() +
                pending.size() +
                node.childCount <=
                settings.maximumNodes;

        if (shouldRefine)
        {
            for (u32 offset = 0U;
                 offset < node.childCount;
                 ++offset)
            {
                pushNode(
                    node.firstChild +
                    offset);
            }

            continue;
        }

        result.push_back({
            .nodeIndex =
                candidate.nodeIndex,
            .integratedEnergy =
                node.integratedEnergy,
            .centroidUv =
                node.centroidUv,
            .projectedWidthPixels =
                candidate.projectedPixels,
            .importance =
                candidate.importance
        });

        if (result.size() >=
            settings.maximumNodes)
        {
            break;
        }
    }

    return result;
}

u32 EmissiveHierarchyLeafCount(
    const EmissiveHierarchy& hierarchy) noexcept
{
    u32 count = 0U;

    for (const auto& node :
         hierarchy.nodes)
    {
        if (node.IsLeaf())
        {
            ++count;
        }
    }

    return count;
}
} // namespace orbit::lighting
