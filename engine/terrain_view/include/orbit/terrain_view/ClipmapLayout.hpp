#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/world/Planet.hpp>
#include <orbit/world/WorldPosition.hpp>

#include <vector>

namespace orbit::terrain_view
{
struct ClipmapConfig
{
    u32 levelCount{12};
    u32 gridResolution{129};
    f64 baseSpacingMeters{1.0};
    f64 levelScale{2.0};
    u32 overlapCells{8};
};

struct AdaptiveClipmapCoverageConfig
{
    bool enabled{true};
    f64 altitudeToHalfExtentScale{4.0};
    f64 growThreshold{0.85};
    f64 shrinkThreshold{0.65};
    u32 maximumTier{3};
};

struct ClipmapLevel
{
    u32 index{0};
    u32 gridResolution{0};

    f64 sampleSpacingMeters{0.0};
    f64 terrainFootprintMeters{0.0};

    f64 innerHoleHalfExtentMeters{0.0};
    f64 outerHalfExtentMeters{0.0};

    f64 morphStartHalfExtentMeters{0.0};
    f64 morphEndHalfExtentMeters{0.0};
};

struct ClipmapLayout
{
    world::SurfaceFrame surfaceFrame{};
    std::vector<ClipmapLevel> levels;
};

[[nodiscard]] ClipmapLayout BuildClipmapLayout(
    const ClipmapConfig& config,
    const world::WorldPosition& observer);

[[nodiscard]] f64 ClipmapOuterHalfExtentMeters(
    const ClipmapConfig& config);

[[nodiscard]] ClipmapConfig ClipmapConfigForTier(
    const ClipmapConfig& baseConfig,
    u32 tier);

[[nodiscard]] u32 SelectAdaptiveClipmapTierForHalfExtent(
    const ClipmapConfig& baseConfig,
    const AdaptiveClipmapCoverageConfig& adaptiveConfig,
    f64 demandedHalfExtentMeters,
    u32 currentTier);

[[nodiscard]] u32 SelectAdaptiveClipmapTier(
    const ClipmapConfig& baseConfig,
    const AdaptiveClipmapCoverageConfig& adaptiveConfig,
    f64 altitudeMeters,
    u32 currentTier);

[[nodiscard]] f64 LodMorphFactor(
    const ClipmapLevel& level,
    f64 maxAbsOffsetMeters) noexcept;
} // namespace orbit::terrain_view
