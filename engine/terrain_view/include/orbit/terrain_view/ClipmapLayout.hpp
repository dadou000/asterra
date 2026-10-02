#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/world/Planet.hpp>
#include <orbit/world/WorldPosition.hpp>

#include <array>
#include <vector>

namespace orbit::terrain_view
{
// Most distance bands a clipmap can have.
inline constexpr u32 kMaxClipmapBands = 16U;

struct ClipmapConfig
{
    u32 levelCount{12};
    u32 gridResolution{129};
    f64 baseSpacingMeters{1.0};
    f64 levelScale{2.0};
    u32 overlapCells{8};

    // Coarse levels can carry a denser grid than fine ones. A level whose sample
    // spacing is at least coarseMinSpacingMeters uses coarseGridResolution
    // samples per axis instead of gridResolution (0 keeps gridResolution
    // everywhere). Seen from orbit the rings are far coarser than the screen
    // needs, so the few coarse levels spend more samples; the many fine levels
    // near the ground stay small. Both must be 4k+1.
    u32 coarseGridResolution{0};
    f64 coarseMinSpacingMeters{0.0};

    // EXPERIMENTAL distance-banded clipmap. With bandCount > 0, level k is the
    // part of the terrain whose distance from the camera lies between
    // bandEdgesMeters[k - 1] (0 for level 0) and bandEdgesMeters[k] (the last
    // edge is the farthest distance drawn); levelCount, baseSpacingMeters and levelScale are
    // ignored. Each level's window covers its outer edge plus bandExtentMargin and
    // its sample spacing is that half extent over the half grid, so spacing
    // follows the band instead of a fixed 2:1 ladder. Neighbouring levels overlap
    // by bandZoneFraction of the edge distance on each side; the renderer
    // cross-fades them there per pixel, so the rings resize with the camera.
    u32 bandCount{0};
    std::array<f64, kMaxClipmapBands> bandEdgesMeters{};
    f64 bandExtentMargin{1.3};
    f64 bandZoneFraction{0.15};
    // Banded levels refresh only the strip that scrolled into view. False
    // regenerates the whole level on every scroll (for comparison).
    bool bandPartialUpdates{true};

    [[nodiscard]] constexpr bool Banded() const noexcept { return bandCount > 0U; }
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
    // False for levels outside the camera's ClipmapPlan: they are neither
    // drawn nor generated. BuildClipmapLayout returns every level active.
    bool active{true};
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

// Levels in the clipmap (bandCount when banded, otherwise levelCount).
[[nodiscard]] u32 ClipmapLevelCount(const ClipmapConfig& config) noexcept;

// Sample spacing of one level.
[[nodiscard]] f64 ClipmapLevelSpacingMeters(
    const ClipmapConfig& config,
    u32 levelIndex) noexcept;

// Camera-distance band of a banded level: [inner, outer]. The last level's outer
// edge is the farthest distance the clipmap draws.
struct ClipmapBand
{
    f64 innerMeters{0.0};
    f64 outerMeters{0.0};
};
[[nodiscard]] ClipmapBand ClipmapLevelBand(
    const ClipmapConfig& config,
    u32 levelIndex) noexcept;

// Samples per axis of one level (gridResolution, or coarseGridResolution for a
// coarse level).
[[nodiscard]] u32 ClipmapLevelGridResolution(
    const ClipmapConfig& config,
    u32 levelIndex) noexcept;

// Half extent of one level (half its cells times its sample spacing).
[[nodiscard]] f64 ClipmapLevelHalfExtentMeters(
    const ClipmapConfig& config,
    u32 levelIndex) noexcept;

[[nodiscard]] ClipmapLayout BuildClipmapLayout(
    const ClipmapConfig& config,
    const world::WorldPosition& observer);

// Restricts a layout to levels [firstLevel, lastLevel] (inclusive). The finest
// active level loses its inner hole, since no finer level covers the centre.
void ApplyClipmapActiveRange(
    ClipmapLayout& layout,
    u32 firstLevel,
    u32 lastLevel);

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

/**
 * Returns true only when an entire coarse clipmap cell is contained by the
 * finer level's guaranteed inner coverage.
 *
 * The finer/coarser centers can differ by half a coarse cell. Culling from the
 * cell center alone can therefore remove a coarse cell whose outer half is not
 * covered by the finer level, producing rectangular cracks. This coverage-first
 * rule mirrors Asterra's proven Godot terrain handoff: the parent is removed
 * only after the child fully covers the region.
 */
[[nodiscard]] bool ClipmapCellFullyInsideInnerHole(
    const ClipmapLevel& level,
    f64 cellCenterXMeters,
    f64 cellCenterYMeters,
    f64 holeCenterXMeters,
    f64 holeCenterYMeters) noexcept;
} // namespace orbit::terrain_view
