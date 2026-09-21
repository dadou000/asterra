#include <orbit/lighting/EmissiveSampling.hpp>

#include <array>
#include <cmath>

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    constexpr u32 width = 16U;
    constexpr u32 height = 16U;

    std::array<
        math::Float3,
        width * height>
        pixels{};

    for (auto& pixel : pixels)
    {
        pixel = {0.05F, 0.05F, 0.05F};
    }

    pixels[3U * width + 5U] =
        {80.0F, 5.0F, 1.0F};

    const frames::FrameId frame{
        .high = 1U,
        .low = 2U};
    const universe::BodyId body{
        .high = 3U,
        .low = 4U};

    const auto hierarchy =
        BuildEmissiveHierarchy(
            {
                .frame = frame,
                .body = body,
                .stableId = 77U,
                .originInFrameMeters =
                    {-1.0, -1.0, 0.0},
                .axisUInFrameMeters =
                    {2.0, 0.0, 0.0},
                .axisVInFrameMeters =
                    {0.0, 2.0, 0.0},
                .width = width,
                .height = height,
                .giRadiance = pixels
            },
            {
                .leafTileWidth = 2U,
                .leafTileHeight = 2U
            });

    LightingView view;
    view.frame = frame;
    view.body = body;
    view.cameraPositionInFrameMeters =
        {0.0, 0.0, -150.0};
    view.verticalFovRadians = 1.0F;

    const auto samples =
        BuildEmissiveSampleSet(
            hierarchy,
            view,
            1920U,
            1080U,
            {
                .subdivisionProjectedPixels =
                    32.0F,
                .smallEmitterPeakLuminance =
                    10.0,
                .smallEmitterContrast =
                    4.0F,
                .promotedMaximumProjectedPixels =
                    8.0F,
                .maximumSamples =
                    256U
            });

    if (samples.emitters.empty() ||
        !samples.EnergyPartitionValid())
    {
        return 1;
    }

    bool promoted = false;
    f32 probabilitySum = 0.0F;

    for (const auto& emitter :
         samples.emitters)
    {
        promoted |=
            emitter.promotedSmallEmitter;

        probabilitySum +=
            emitter.samplingProbability;
    }

    if (!promoted ||
        std::abs(
            probabilitySum -
            1.0F) >
            1.0e-4F)
    {
        return 2;
    }

    // The sampling set partitions the hierarchy. Promoting the LED does not
    // add its energy on top of an aggregate parent.
    f64 represented = 0.0;

    for (const auto& emitter :
         samples.emitters)
    {
        represented +=
            emitter.radiantImportance;
    }

    if (std::abs(
            represented -
            hierarchy.nodes[
                hierarchy.root].
                radiantImportance) >
        1.0e-5 *
            std::max(
                hierarchy.nodes[
                    hierarchy.root].
                    radiantImportance,
                1.0))
    {
        return 3;
    }

    const auto budgetedA =
        SelectEmissiveSamplesForBudget(
            samples,
            {
                .maximumSamples = 4U,
                .sequence = 1234U,
                .preservePromotedEmitters = true
            });

    const auto budgetedB =
        SelectEmissiveSamplesForBudget(
            samples,
            {
                .maximumSamples = 4U,
                .sequence = 1234U,
                .preservePromotedEmitters = true
            });

    if (budgetedA.size() != 4U ||
        budgetedB.size() !=
            budgetedA.size())
    {
        return 4;
    }

    bool retainedPromoted = false;

    for (std::size_t index = 0U;
         index < budgetedA.size();
         ++index)
    {
        const auto& a =
            budgetedA[index];
        const auto& b =
            budgetedB[index];

        retainedPromoted |=
            a.promotedSmallEmitter;

        if (a.sourceStableId !=
                b.sourceStableId ||
            a.nodeIndex !=
                b.nodeIndex ||
            a.estimatorWeight !=
                b.estimatorWeight ||
            !std::isfinite(
                a.estimatorWeight) ||
            a.estimatorWeight <= 0.0F)
        {
            return 5;
        }
    }

    if (!retainedPromoted)
    {
        return 6;
    }

    // Across deterministic sequence salts the weighted stochastic draws must
    // converge to the residual hierarchy importance rather than systematically
    // losing energy when the scheduler caps the sample count.
    f64 estimatedImportanceSum = 0.0;
    constexpr u32 sequenceCount = 2048U;

    for (u32 sequence = 0U;
         sequence < sequenceCount;
         ++sequence)
    {
        const auto budgeted =
            SelectEmissiveSamplesForBudget(
                samples,
                {
                    .maximumSamples = 4U,
                    .sequence = sequence,
                    .preservePromotedEmitters = true
                });

        f64 estimate = 0.0;

        for (const auto& emitter :
             budgeted)
        {
            estimate +=
                emitter.radiantImportance *
                static_cast<f64>(
                    emitter.estimatorWeight);
        }

        estimatedImportanceSum +=
            estimate;
    }

    const f64 estimatedAverage =
        estimatedImportanceSum /
        static_cast<f64>(
            sequenceCount);

    const f64 expected =
        hierarchy.nodes[
            hierarchy.root].
            radiantImportance;

    if (std::abs(
            estimatedAverage -
            expected) >
        0.08 *
            std::max(
                expected,
                1.0))
    {
        return 7;
    }

    return 0;
}
