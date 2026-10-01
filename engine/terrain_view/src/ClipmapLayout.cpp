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

u32 ClipmapLevelCount(const ClipmapConfig& config) noexcept
{
    return config.Banded() ? config.bandCount : config.levelCount;
}

ClipmapBand ClipmapLevelBand(
    const ClipmapConfig& config,
    const u32 levelIndex) noexcept
{
    ClipmapBand band;
    if (!config.Banded() || levelIndex >= config.bandCount)
        return band;
    band.innerMeters =
        levelIndex == 0U ? 0.0 : config.bandEdgesMeters[levelIndex - 1U];
    band.outerMeters = config.bandEdgesMeters[levelIndex];
    return band;
}

f64 ClipmapLevelSpacingMeters(
    const ClipmapConfig& config,
    const u32 levelIndex) noexcept
{
    if (config.Banded())
    {
        const u32 index = std::min(levelIndex, config.bandCount - 1U);
        const f64 reach = config.bandEdgesMeters[index];
        const u32 half = (config.gridResolution - 1U) / 2U;
        return reach * config.bandExtentMargin / static_cast<f64>(half);
    }
    return config.baseSpacingMeters *
        std::pow(config.levelScale, static_cast<f64>(levelIndex));
}

u32 ClipmapLevelGridResolution(
    const ClipmapConfig& config,
    const u32 levelIndex) noexcept
{
    if (config.Banded())
        return config.gridResolution;
    if (config.coarseGridResolution != 0U &&
        config.baseSpacingMeters *
                std::pow(config.levelScale, static_cast<f64>(levelIndex)) >=
            config.coarseMinSpacingMeters)
    {
        return config.coarseGridResolution;
    }
    return config.gridResolution;
}

f64 ClipmapLevelHalfExtentMeters(
    const ClipmapConfig& config,
    const u32 levelIndex) noexcept
{
    const u32 resolution = ClipmapLevelGridResolution(config, levelIndex);
    return static_cast<f64>((resolution - 1U) / 2U) *
        ClipmapLevelSpacingMeters(config, levelIndex);
}

namespace
{
// The experimental distance-banded layout: independent windows, no holes and no
// geomorph (the renderer cross-fades neighbouring levels per pixel by camera
// distance).
[[nodiscard]] ClipmapLayout BuildBandedClipmapLayout(
    const ClipmapConfig& config,
    const world::WorldPosition& observer)
{
    if (config.bandCount > kMaxClipmapBands)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap has too many distance bands.");
    }
    if (config.gridResolution < 9 || (config.gridResolution % 2U) == 0U)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap grid resolution must be odd and at least 9.");
    }
    if (!std::isfinite(config.bandExtentMargin) ||
        config.bandExtentMargin < 1.0 ||
        !std::isfinite(config.bandZoneFraction) ||
        config.bandZoneFraction < 0.0 || config.bandZoneFraction >= 0.5 ||
        config.bandExtentMargin < 1.0 + config.bandZoneFraction)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap band margin must cover the cross-fade zone.");
    }
    f64 previous = 0.0;
    for (u32 index = 0; index < config.bandCount; ++index)
    {
        const f64 edge = config.bandEdgesMeters[index];
        if (!std::isfinite(edge) || edge <= previous)
        {
            throw std::invalid_argument(
                "Orbit terrain clipmap band edges must be positive and increasing.");
        }
        previous = edge;
    }
    if (config.bandCount < 2U)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap needs at least two distance bands.");
    }

    ClipmapLayout layout{};
    layout.surfaceFrame = world::MakeSurfaceFrame(observer.meters);
    layout.levels.reserve(config.bandCount);
    for (u32 index = 0; index < config.bandCount; ++index)
    {
        const f64 spacing = ClipmapLevelSpacingMeters(config, index);
        const f64 halfExtent = ClipmapLevelHalfExtentMeters(config, index);
        layout.levels.push_back({
            .index = index,
            .gridResolution = config.gridResolution,
            .sampleSpacingMeters = spacing,
            .terrainFootprintMeters = spacing,
            .innerHoleHalfExtentMeters = 0.0,
            .outerHalfExtentMeters = halfExtent,
            .morphStartHalfExtentMeters = halfExtent,
            .morphEndHalfExtentMeters = halfExtent});
    }
    return layout;
}
} // namespace

ClipmapLayout BuildClipmapLayout(
    const ClipmapConfig& config,
    const world::WorldPosition& observer)
{
    const f64 observerLengthSquared =
        math::LengthSquared(observer.meters);

    if (!std::isfinite(observerLengthSquared) ||
        observerLengthSquared <= 0.0)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap layout requires a finite observer direction.");
    }

    if (config.Banded())
        return BuildBandedClipmapLayout(config, observer);

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

    if (!std::isfinite(config.baseSpacingMeters) ||
        config.baseSpacingMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap base spacing must be positive.");
    }

    // This implementation uses the standard 2:1 geometry-clipmap hierarchy.
    // Keeping the ratio fixed is what lets a finer border and its parent grid
    // share exact lattice coordinates while the finer window scrolls
    // toroidally. Supporting arbitrary ratios would require a different
    // transition topology rather than silently accepting misaligned grids.
    if (!std::isfinite(config.levelScale) ||
        std::abs(config.levelScale - 2.0) >
        1.0e-12)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap level scale must be exactly 2.0.");
    }

    // With a 2:1 parent ratio, half the grid must contain an even number of
    // fine cells so both +/- outer borders fall on parent-grid coordinates.
    // Common clipmap sizes (9, 17, 33, 65, 129, ...) satisfy this 4k+1 rule.
    for (const u32 resolution :
         {config.gridResolution, config.coarseGridResolution})
    {
        if (resolution != 0U && ((resolution - 1U) % 4U) != 0U)
        {
            throw std::invalid_argument(
                "Orbit terrain clipmap grid resolution must be 4k+1 for 2:1 LOD alignment.");
        }
    }
    if (config.coarseGridResolution != 0U &&
        (config.coarseGridResolution < config.gridResolution ||
         !std::isfinite(config.coarseMinSpacingMeters) ||
         config.coarseMinSpacingMeters < 0.0))
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap coarse grid must be at least the fine grid and have a finite spacing threshold.");
    }
    const u32 baseHalfCells = (config.gridResolution - 1U) / 2U;

    if (config.overlapCells == 0 ||
        config.overlapCells >= baseHalfCells)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap overlap must fit inside half the grid.");
    }

    ClipmapLayout layout{};
    layout.surfaceFrame =
        world::MakeSurfaceFrame(observer.meters);
    layout.levels.reserve(config.levelCount);

    for (u32 levelIndex = 0;
         levelIndex < config.levelCount;
         ++levelIndex)
    {
        const f64 spacing =
            config.baseSpacingMeters *
            std::pow(
                config.levelScale,
                static_cast<f64>(levelIndex));

        const u32 levelResolution =
            ClipmapLevelGridResolution(config, levelIndex);
        const u32 halfCells = (levelResolution - 1U) / 2U;

        const f64 outerHalfExtent =
            static_cast<f64>(halfCells) * spacing;

        // The morph band keeps the same share of the half extent on every level.
        const f64 overlapCells = std::max(
            static_cast<f64>(config.overlapCells),
            static_cast<f64>(config.overlapCells) *
                static_cast<f64>(halfCells) / static_cast<f64>(baseHalfCells));
        const f64 overlapWidth = overlapCells * spacing;

        if (!std::isfinite(spacing) ||
            !std::isfinite(outerHalfExtent) ||
            !std::isfinite(overlapWidth))
        {
            throw std::overflow_error(
                "Orbit terrain clipmap layout exceeds finite range.");
        }

        // The parent ring starts only outside the complete finer patch.
        // The finer level performs its geomorph inside its own outer band and
        // reaches parent geometry at the outer edge. Keeping the coarse parent
        // hidden beneath that whole patch avoids the overlapping coplanar
        // surfaces that previously required "seam sinking" and produced large
        // rectangular terraces. This mirrors the proven Godot quadtree handoff:
        // once a child covers an area, its parent is not drawn underneath it.
        const f64 innerHoleHalfExtent =
            levelIndex == 0
                ? 0.0
                : layout.levels.back().
                    outerHalfExtentMeters;

        const f64 morphStart =
            std::max(
                innerHoleHalfExtent,
                outerHalfExtent - overlapWidth);

        layout.levels.push_back({
            .index = levelIndex,
            .gridResolution = levelResolution,
            .sampleSpacingMeters = spacing,
            .terrainFootprintMeters = spacing,
            .innerHoleHalfExtentMeters = innerHoleHalfExtent,
            .outerHalfExtentMeters = outerHalfExtent,
            .morphStartHalfExtentMeters = morphStart,
            .morphEndHalfExtentMeters = outerHalfExtent
        });
    }

    return layout;
}

void ApplyClipmapActiveRange(
    ClipmapLayout& layout,
    const u32 firstLevel,
    const u32 lastLevel)
{
    for (ClipmapLevel& level : layout.levels)
    {
        level.active = level.index >= firstLevel && level.index <= lastLevel;
        if (level.index == firstLevel)
        {
            // Nothing finer sits in the middle: draw the whole patch. The
            // morph band already starts outside the hole, so it is unchanged.
            level.innerHoleHalfExtentMeters = 0.0;
        }
    }
}

f64 ClipmapOuterHalfExtentMeters(
    const ClipmapConfig& config)
{
    if (config.Banded())
        return ClipmapLevelHalfExtentMeters(config, config.bandCount - 1U);

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

    if (!std::isfinite(config.levelScale) ||
        std::abs(config.levelScale - 2.0) >
            1.0e-12)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap coverage requires a 2:1 level scale.");
    }

    const u32 cellsPerAxis =
        config.gridResolution - 1U;

    if ((cellsPerAxis % 4U) != 0U)
    {
        throw std::invalid_argument(
            "Orbit terrain clipmap coverage requires a 4k+1 grid resolution.");
    }

    const f64 extent =
        ClipmapLevelHalfExtentMeters(
            config,
            config.levelCount - 1U);

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

bool ClipmapCellFullyInsideInnerHole(
    const ClipmapLevel& level,
    const f64 cellCenterXMeters,
    const f64 cellCenterYMeters,
    const f64 holeCenterXMeters,
    const f64 holeCenterYMeters) noexcept
{
    if (level.innerHoleHalfExtentMeters <= 0.0 ||
        level.sampleSpacingMeters <= 0.0)
    {
        return false;
    }

    const f64 halfCell =
        level.sampleSpacingMeters * 0.5;

    // Use <= intentionally: a cell whose outer edge lands exactly on the
    // finer guaranteed-coverage boundary is safe to remove.
    return
        std::abs(
            cellCenterXMeters -
            holeCenterXMeters) +
                halfCell <=
            level.innerHoleHalfExtentMeters &&
        std::abs(
            cellCenterYMeters -
            holeCenterYMeters) +
                halfCell <=
            level.innerHoleHalfExtentMeters;
}
} // namespace orbit::terrain_view
