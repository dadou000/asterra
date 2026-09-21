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

std::vector<EmissiveSampledEmitter>
SelectEmissiveSamplesForBudget(
    const EmissiveSampleSet& source,
    const EmissiveBudgetSelectionConfig& config)
{
    std::vector<EmissiveSampledEmitter> result;

    if (config.maximumSamples == 0U ||
        source.emitters.empty())
    {
        return result;
    }

    if (source.emitters.size() <=
        config.maximumSamples)
    {
        result = source.emitters;

        for (auto& emitter : result)
        {
            emitter.estimatorWeight = 1.0F;
        }

        return result;
    }

    result.reserve(
        config.maximumSamples);

    std::vector<std::size_t> remaining;
    remaining.reserve(
        source.emitters.size());

    if (config.preservePromotedEmitters)
    {
        std::vector<std::size_t> promoted;

        for (std::size_t index = 0U;
             index < source.emitters.size();
             ++index)
        {
            if (source.emitters[index].
                    promotedSmallEmitter)
            {
                promoted.push_back(index);
            }
            else
            {
                remaining.push_back(index);
            }
        }

        std::stable_sort(
            promoted.begin(),
            promoted.end(),
            [&source](
                const std::size_t a,
                const std::size_t b)
            {
                const auto& ea =
                    source.emitters[a];
                const auto& eb =
                    source.emitters[b];

                if (ea.radiantImportance !=
                    eb.radiantImportance)
                {
                    return
                        ea.radiantImportance >
                        eb.radiantImportance;
                }

                if (ea.sourceStableId !=
                    eb.sourceStableId)
                {
                    return
                        ea.sourceStableId <
                        eb.sourceStableId;
                }

                return
                    ea.nodeIndex <
                    eb.nodeIndex;
            });

        const std::size_t retainCount =
            std::min<std::size_t>(
                promoted.size(),
                config.maximumSamples);

        for (std::size_t i = 0U;
             i < retainCount;
             ++i)
        {
            auto emitter =
                source.emitters[
                    promoted[i]];
            emitter.estimatorWeight =
                1.0F;
            result.push_back(
                emitter);
        }

        // If promoted emitters alone saturate the budget there is no safe
        // stochastic room left. Keeping the strongest ones deterministic is
        // preferable to flickering tiny high-energy sources.
        if (result.size() >=
            config.maximumSamples)
        {
            return result;
        }

        for (std::size_t i = retainCount;
             i < promoted.size();
             ++i)
        {
            remaining.push_back(
                promoted[i]);
        }
    }
    else
    {
        remaining.resize(
            source.emitters.size());

        for (std::size_t index = 0U;
             index < remaining.size();
             ++index)
        {
            remaining[index] =
                index;
        }
    }

    const u32 stochasticDraws =
        config.maximumSamples -
        static_cast<u32>(
            result.size());

    if (stochasticDraws == 0U ||
        remaining.empty())
    {
        return result;
    }

    f64 totalImportance = 0.0;

    for (const auto index : remaining)
    {
        totalImportance +=
            std::max(
                source.emitters[index].
                    radiantImportance,
                0.0);
    }

    if (!(totalImportance > 0.0) ||
        !std::isfinite(totalImportance))
    {
        return result;
    }

    const auto random01 =
        [](u64 value) noexcept
        {
            // SplitMix64 finalizer. Use the high 53 bits to produce a stable
            // double in [0,1).
            value +=
                0x9e3779b97f4a7c15ULL;
            value =
                (value ^
                 (value >> 30U)) *
                0xbf58476d1ce4e5b9ULL;
            value =
                (value ^
                 (value >> 27U)) *
                0x94d049bb133111ebULL;
            value ^=
                value >> 31U;

            return
                static_cast<f64>(
                    value >> 11U) *
                (1.0 /
                 9007199254740992.0);
        };

    for (u32 draw = 0U;
         draw < stochasticDraws;
         ++draw)
    {
        const f64 target =
            random01(
                config.sequence ^
                (static_cast<u64>(draw) *
                 0xd1342543de82ef95ULL)) *
            totalImportance;

        f64 cumulative = 0.0;
        std::size_t chosen =
            remaining.back();

        for (const auto index : remaining)
        {
            cumulative +=
                std::max(
                    source.emitters[index].
                        radiantImportance,
                    0.0);

            if (target < cumulative)
            {
                chosen = index;
                break;
            }
        }

        auto emitter =
            source.emitters[chosen];

        const f64 probability =
            std::max(
                emitter.radiantImportance,
                0.0) /
            totalImportance;

        if (!(probability > 0.0) ||
            !std::isfinite(probability))
        {
            continue;
        }

        emitter.samplingProbability =
            static_cast<f32>(
                probability);

        emitter.estimatorWeight =
            static_cast<f32>(
                1.0 /
                (static_cast<f64>(
                     stochasticDraws) *
                 probability));

        result.push_back(
            emitter);
    }

    return result;
}

} // namespace orbit::lighting
