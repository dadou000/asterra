#include <orbit/lighting/EmissiveSampling.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] f64 Luminance(
    const math::Float3 value) noexcept
{
    return
        0.2126 *
            static_cast<f64>(
                std::max(value.x, 0.0F)) +
        0.7152 *
            static_cast<f64>(
                std::max(value.y, 0.0F)) +
        0.0722 *
            static_cast<f64>(
                std::max(value.z, 0.0F));
}

[[nodiscard]] f32 ProjectedPixels(
    const EmissiveHierarchyNode& node,
    const EmissiveHierarchy& hierarchy,
    const LightingView& view,
    const u32 viewportWidth,
    const u32 viewportHeight) noexcept
{
    if (viewportWidth == 0U ||
        viewportHeight == 0U)
    {
        return 0.0F;
    }

    const auto delta =
        node.centerInFrameMeters -
        view.cameraPositionInFrameMeters;

    const f64 distanceSquared =
        math::LengthSquared(delta);

    if (!std::isfinite(distanceSquared) ||
        distanceSquared <= 1.0e-12)
    {
        return static_cast<f32>(
            viewportWidth *
            viewportHeight);
    }

    const f64 distance =
        std::sqrt(distanceSquared);

    const auto direction =
        delta / distance;

    const f64 facing =
        std::abs(
            math::Dot(
                hierarchy.surfaceNormal,
                direction));

    const f64 projectedArea =
        node.areaMetersSquared *
        facing /
        distanceSquared;

    const f64 focalPixels =
        static_cast<f64>(
            viewportHeight) /
        (2.0 *
         std::tan(
             static_cast<f64>(
                 view.verticalFovRadians) *
             0.5));

    return static_cast<f32>(
        std::sqrt(
            std::max(
                projectedArea *
                    focalPixels *
                    focalPixels,
                0.0)));
}

[[nodiscard]] f32 Contrast(
    const EmissiveHierarchyNode& node) noexcept
{
    const f64 average =
        Luminance(
            node.averageRadiance);

    if (average <= 1.0e-12)
    {
        return
            node.peakLuminance > 0.0
                ? std::numeric_limits<f32>::
                    infinity()
                : 1.0F;
    }

    return static_cast<f32>(
        node.peakLuminance /
        average);
}
} // namespace

bool EmissiveSampleSet::EnergyPartitionValid(
    const f64 relativeTolerance) const noexcept
{
    if (!std::isfinite(totalImportance) ||
        !std::isfinite(representedImportance) ||
        totalImportance < 0.0 ||
        representedImportance < 0.0)
    {
        return false;
    }

    const f64 scale =
        std::max(
            totalImportance,
            1.0);

    return
        std::abs(
            representedImportance -
            totalImportance) <=
        std::max(
            relativeTolerance,
            0.0) *
            scale;
}

EmissiveSampleSet BuildEmissiveSampleSet(
    const EmissiveHierarchy& hierarchy,
    const LightingView& view,
    const u32 viewportWidth,
    const u32 viewportHeight,
    const EmissiveSamplingConfig& config)
{
    EmissiveSampleSet result;

    if (hierarchy.root >=
            hierarchy.nodes.size() ||
        hierarchy.frame != view.frame ||
        hierarchy.body != view.body ||
        config.maximumSamples == 0U)
    {
        return result;
    }

    result.totalImportance =
        hierarchy.nodes[
            hierarchy.root].
            radiantImportance;

    std::vector<u32> stack{
        hierarchy.root};

    while (!stack.empty())
    {
        const u32 nodeIndex =
            stack.back();
        stack.pop_back();

        const auto& node =
            hierarchy.nodes[nodeIndex];

        const f32 projected =
            ProjectedPixels(
                node,
                hierarchy,
                view,
                viewportWidth,
                viewportHeight);

        const f32 contrast =
            Contrast(node);

        const bool refineSpatial =
            projected >=
                config.
                    subdivisionProjectedPixels;

        const bool refineSmallBright =
            node.peakLuminance >=
                config.
                    smallEmitterPeakLuminance &&
            contrast >=
                config.
                    smallEmitterContrast;

        const bool budgetAllowsChildren =
            result.emitters.size() +
                stack.size() +
                node.childCount <=
            config.maximumSamples;

        if (!node.IsLeaf() &&
            budgetAllowsChildren &&
            (refineSpatial ||
             refineSmallBright))
        {
            for (u32 child = 0U;
                 child < node.childCount;
                 ++child)
            {
                stack.push_back(
                    node.children[
                        node.childCount -
                        1U -
                        child]);
            }

            continue;
        }

        const bool promoted =
            node.peakLuminance >=
                config.
                    smallEmitterPeakLuminance &&
            contrast >=
                config.
                    smallEmitterContrast &&
            projected <=
                config.
                    promotedMaximumProjectedPixels;

        // Use the energy centroid rather than the region midpoint so a tiny
        // LED inside an 8x8 leaf does not jump to the leaf center.
        const math::Double3 position =
            node.energyCentroidInFrameMeters;

        result.emitters.push_back({
            .sourceStableId =
                hierarchy.stableId,
            .nodeIndex =
                nodeIndex,
            .positionInFrameMeters =
                position,
            .normalInFrame =
                hierarchy.surfaceNormal,
            .averageRadiance =
                node.averageRadiance,
            .integratedRadianceArea =
                node.integratedRadianceArea,
            .areaMetersSquared =
                node.areaMetersSquared,
            .radiantImportance =
                node.radiantImportance,
            .projectedPixels =
                projected,
            .promotedSmallEmitter =
                promoted
        });

        result.representedImportance +=
            node.radiantImportance;
    }

    if (result.totalImportance > 1.0e-20)
    {
        for (auto& emitter :
             result.emitters)
        {
            emitter.samplingProbability =
                static_cast<f32>(
                    emitter.radiantImportance /
                    result.totalImportance);
        }
    }

    return result;
}
} // namespace orbit::lighting
