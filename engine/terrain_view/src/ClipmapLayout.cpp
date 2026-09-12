#include <orbit/terrain_view/ClipmapLayout.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::terrain_view
{
namespace
{
[[nodiscard]] f64 SmoothStep01(const f64 value) noexcept
{
    const f64 x = std::clamp(value, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}
} // namespace

ClipmapLayout BuildClipmapLayout(
    const ClipmapConfig& config,
    const world::WorldPosition& observer)
{
    if (config.levelCount == 0)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmaps require at least one level.");
    }

    if (config.gridResolution < 9 ||
        (config.gridResolution % 2U) == 0U)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap grid resolution must be odd and at least 9.");
    }

    if (config.baseSpacingMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap base spacing must be positive.");
    }

    if (config.levelScale <= 1.0)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap level scale must be greater than one.");
    }

    const u32 cellsPerAxis = config.gridResolution - 1U;
    const u32 halfCells = cellsPerAxis / 2U;

    if (config.overlapCells == 0 ||
        config.overlapCells >= halfCells)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap overlap must fit inside half the grid.");
    }

    ClipmapLayout layout{};
    layout.surfaceFrame =
        world::MakeSurfaceFrame(observer.meters);
    layout.levels.reserve(config.levelCount);

    f64 previousOuterHalfExtent = 0.0;

    for (u32 levelIndex = 0;
         levelIndex < config.levelCount;
         ++levelIndex)
    {
        const f64 spacing =
            config.baseSpacingMeters *
            std::pow(
                config.levelScale,
                static_cast<f64>(levelIndex));

        const f64 outerHalfExtent =
            static_cast<f64>(halfCells) * spacing;

        const f64 overlapWidth =
            static_cast<f64>(config.overlapCells) * spacing;

        const f64 innerHoleHalfExtent =
            levelIndex == 0
                ? 0.0
                : std::max(
                    0.0,
                    previousOuterHalfExtent - overlapWidth);

        const f64 morphStart =
            std::max(
                innerHoleHalfExtent,
                outerHalfExtent - overlapWidth);

        layout.levels.push_back({
            .index = levelIndex,
            .gridResolution = config.gridResolution,
            .sampleSpacingMeters = spacing,
            .terrainFootprintMeters = spacing,
            .innerHoleHalfExtentMeters = innerHoleHalfExtent,
            .outerHalfExtentMeters = outerHalfExtent,
            .morphStartHalfExtentMeters = morphStart,
            .morphEndHalfExtentMeters = outerHalfExtent
        });

        previousOuterHalfExtent = outerHalfExtent;
    }

    return layout;
}

f64 ClipmapOuterHalfExtentMeters(
    const ClipmapConfig& config)
{
    if (config.levelCount == 0)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap coverage requires at least one level.");
    }

    if (config.gridResolution < 9 ||
        (config.gridResolution % 2U) == 0U)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap coverage requires an odd grid resolution of at least 9.");
    }

    if (config.baseSpacingMeters <= 0.0 ||
        !std::isfinite(config.baseSpacingMeters))
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap coverage requires a finite positive base spacing.");
    }

    if (config.levelScale <= 1.0 ||
        !std::isfinite(config.levelScale))
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap coverage requires a finite level scale greater than one.");
    }

    const u32 halfCells =
        (config.gridResolution - 1U) / 2U;

    const f64 coarsestSpacing =
        config.baseSpacingMeters *
        std::pow(
            config.levelScale,
            static_cast<f64>(
                config.levelCount - 1U));

    const f64 extent =
        static_cast<f64>(halfCells) *
        coarsestSpacing;

    if (!std::isfinite(extent))
    {
        throw std::overflow_error(
            "Orbit terrain clipmap coverage exceeds finite range.");
    }

    return extent;
}

ClipmapConfig ClipmapConfigForTier(
    const ClipmapConfig& baseConfig,
    const u32 tier)
{
    static_cast<void>(
        ClipmapOuterHalfExtentMeters(
            baseConfig));

    ClipmapConfig result =
        baseConfig;

    result.baseSpacingMeters *=
        std::pow(
            baseConfig.levelScale,
            static_cast<f64>(tier));

    if (!std::isfinite(
            result.baseSpacingMeters))
    {
        throw std::overflow_error(
            "Orbit adaptive terrain clipmap spacing exceeds finite range.");
    }

    return result;
}

u32 SelectAdaptiveClipmapTierForHalfExtent(
    const ClipmapConfig& baseConfig,
    const AdaptiveClipmapCoverageConfig& adaptiveConfig,
    const f64 demandedHalfExtentMeters,
    const u32 currentTier)
{
    static_cast<void>(
        ClipmapOuterHalfExtentMeters(
            baseConfig));

    if (!adaptiveConfig.enabled)
    {
        return 0;
    }

    if (!std::isfinite(
            demandedHalfExtentMeters) ||
        demandedHalfExtentMeters < 0.0 ||
        !std::isfinite(
            adaptiveConfig.growThreshold) ||
        !std::isfinite(
            adaptiveConfig.shrinkThreshold) ||
        adaptiveConfig.shrinkThreshold <=
            0.0 ||
        adaptiveConfig.growThreshold <=
            adaptiveConfig.shrinkThreshold ||
        adaptiveConfig.growThreshold > 1.0)
    {
        throw std::invalid_argument(
            "Orbit adaptive terrain clipmap coverage configuration is invalid.");
    }

    u32 tier =
        std::min(
            currentTier,
            adaptiveConfig.maximumTier);

    while (tier <
           adaptiveConfig.maximumTier)
    {
        const f64 currentExtent =
            ClipmapOuterHalfExtentMeters(
                ClipmapConfigForTier(
                    baseConfig,
                    tier));

        if (demandedHalfExtentMeters <=
            currentExtent *
                adaptiveConfig.
                    growThreshold)
        {
            break;
        }

        ++tier;
    }

    while (tier > 0)
    {
        const f64 finerExtent =
            ClipmapOuterHalfExtentMeters(
                ClipmapConfigForTier(
                    baseConfig,
                    tier - 1U));

        if (demandedHalfExtentMeters >=
            finerExtent *
                adaptiveConfig.
                    shrinkThreshold)
        {
            break;
        }

        --tier;
    }

    return tier;
}

u32 SelectAdaptiveClipmapTier(
    const ClipmapConfig& baseConfig,
    const AdaptiveClipmapCoverageConfig& adaptiveConfig,
    const f64 altitudeMeters,
    const u32 currentTier)
{
    if (!std::isfinite(altitudeMeters) ||
        altitudeMeters < 0.0 ||
        !std::isfinite(
            adaptiveConfig.
                altitudeToHalfExtentScale) ||
        adaptiveConfig.
                altitudeToHalfExtentScale <=
            0.0)
    {
        throw std::invalid_argument(
            "Orbit adaptive terrain clipmap altitude mapping is invalid.");
    }

    return SelectAdaptiveClipmapTierForHalfExtent(
        baseConfig,
        adaptiveConfig,
        altitudeMeters *
            adaptiveConfig.
                altitudeToHalfExtentScale,
        currentTier);
}

f64 LodMorphFactor(
    const ClipmapLevel& level,
    const f64 maxAbsOffsetMeters) noexcept
{
    if (level.morphEndHalfExtentMeters <=
        level.morphStartHalfExtentMeters)
    {
        return 1.0;
    }

    const f64 normalized =
        (std::abs(maxAbsOffsetMeters) -
         level.morphStartHalfExtentMeters) /
        (level.morphEndHalfExtentMeters -
         level.morphStartHalfExtentMeters);

    return SmoothStep01(normalized);
}
} // namespace orbit::terrain_view
