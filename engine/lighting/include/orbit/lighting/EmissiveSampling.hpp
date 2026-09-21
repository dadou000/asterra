#pragma once

#include <orbit/lighting/EmissiveHierarchy.hpp>

#include <vector>

namespace orbit::lighting
{
struct EmissiveSamplingConfig
{
    // Ordinary spatial-detail refinement.
    f32 subdivisionProjectedPixels{16.0F};

    // Compact source nodes brighter than this are refined even when their
    // projected footprint is small.
    f64 smallEmitterPeakLuminance{4.0};

    // Require peak/average contrast so uniformly bright large panels can still
    // collapse while individual LED/highlight regions are localized.
    f32 smallEmitterContrast{2.0F};

    // Below this projected size, a bright terminal node is tagged promoted.
    f32 promotedMaximumProjectedPixels{8.0F};

    u32 maximumSamples{2048U};
};

struct EmissiveSampledEmitter
{
    u64 sourceStableId{0U};
    u32 nodeIndex{0U};

    math::Double3 positionInFrameMeters{};
    math::Double3 normalInFrame{};

    math::Float3 averageRadiance{};
    math::Float3 integratedRadianceArea{};

    f64 areaMetersSquared{0.0};
    f64 radiantImportance{0.0};

    f32 projectedPixels{0.0F};
    f32 samplingProbability{0.0F};

    // Multiplicative Monte-Carlo correction used when M10 budgets require
    // stochastic subsampling. Deterministically retained emitters use 1.
    f32 estimatorWeight{1.0F};

    bool promotedSmallEmitter{false};
};

struct EmissiveSampleSet
{
    std::vector<EmissiveSampledEmitter> emitters;
    f64 totalImportance{0.0};
    f64 representedImportance{0.0};

    [[nodiscard]] bool EnergyPartitionValid(
        f64 relativeTolerance = 1.0e-5) const noexcept;
};

[[nodiscard]] EmissiveSampleSet
BuildEmissiveSampleSet(
    const EmissiveHierarchy& hierarchy,
    const LightingView& view,
    u32 viewportWidth,
    u32 viewportHeight,
    const EmissiveSamplingConfig& config = {});


struct EmissiveBudgetSelectionConfig
{
    u32 maximumSamples{256U};

    // Stable frame/sequence salt. Equal input + sequence gives identical
    // selections, which is useful for deterministic captures/tests.
    u64 sequence{0U};

    bool preservePromotedEmitters{true};
};

[[nodiscard]] std::vector<EmissiveSampledEmitter>
SelectEmissiveSamplesForBudget(
    const EmissiveSampleSet& source,
    const EmissiveBudgetSelectionConfig& config = {});
} // namespace orbit::lighting
