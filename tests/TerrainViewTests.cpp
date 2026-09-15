#include <orbit/math/Vector.hpp>
#include <orbit/terrain_view/ClipmapLayout.hpp>
#include <orbit/terrain_view/ClipmapTracker.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace
{
bool NearlyEqual(
    const orbit::f64 a,
    const orbit::f64 b,
    const orbit::f64 epsilon)
{
    return std::abs(a - b) <= epsilon;
}
} // namespace

int main()
{
    const orbit::world::PlanetDefinition planet{
        .radiusMeters = 6'000'000.0
    };

    const orbit::math::Double3 seamDirection =
        orbit::math::Normalize(orbit::math::Double3{1.0, 0.2, 1.0});

    const orbit::world::SurfaceFrame frame =
        orbit::world::MakeSurfaceFrame(seamDirection);

    if (!NearlyEqual(
            orbit::math::Dot(frame.east, frame.north),
            0.0,
            1.0e-12) ||
        !NearlyEqual(
            orbit::math::Dot(frame.east, frame.up),
            0.0,
            1.0e-12) ||
        !NearlyEqual(
            orbit::math::Dot(frame.north, frame.up),
            0.0,
            1.0e-12))
    {
        std::cerr << "Surface frame is not orthogonal.\n";
        return 1;
    }

    constexpr orbit::f64 eastOffset = 25'000.0;

    const orbit::world::SurfaceFrame movedFrame =
        orbit::world::SurfaceFrameAtOffset(
            planet,
            frame,
            {eastOffset, 0.0});

    const orbit::f64 angularDistance = std::acos(
        std::clamp(
            orbit::math::Dot(frame.up, movedFrame.up),
            -1.0,
            1.0));

    const orbit::f64 recoveredDistance =
        angularDistance * planet.radiusMeters;

    if (!NearlyEqual(
            recoveredDistance,
            eastOffset,
            1.0e-6))
    {
        std::cerr << "Spherical surface offset lost geodesic distance.\n";
        return 1;
    }

    const orbit::math::Double2 inverseOffset =
        orbit::world::SurfaceOffsetBetweenDirections(
            planet,
            frame,
            movedFrame.up);

    if (!NearlyEqual(inverseOffset.x, eastOffset, 1.0e-6) ||
        !NearlyEqual(inverseOffset.y, 0.0, 1.0e-6))
    {
        std::cerr << "Spherical log map did not invert surface motion.\n";
        return 1;
    }

    const orbit::world::WorldPosition observer{
        .meters = seamDirection * (planet.radiusMeters + 1'000.0)
    };

    const orbit::terrain_view::ClipmapConfig config{
        .levelCount = 8,
        .gridResolution = 129,
        .baseSpacingMeters = 100.0,
        .levelScale = 2.0,
        .overlapCells = 8
    };

    const orbit::terrain_view::ClipmapLayout layout =
        orbit::terrain_view::BuildClipmapLayout(
            config,
            observer);

    if (layout.levels.size() != config.levelCount)
    {
        std::cerr << "Unexpected clipmap level count.\n";
        return 1;
    }

    for (orbit::u32 index = 0;
         index < config.levelCount;
         ++index)
    {
        const auto& level = layout.levels[index];

        const orbit::f64 expectedSpacing =
            config.baseSpacingMeters *
            std::pow(
                config.levelScale,
                static_cast<orbit::f64>(index));

        if (!NearlyEqual(
                level.sampleSpacingMeters,
                expectedSpacing,
                1.0e-12))
        {
            std::cerr << "Clipmap spacing progression failed.\n";
            return 1;
        }

        if (level.morphStartHalfExtentMeters >=
            level.morphEndHalfExtentMeters)
        {
            std::cerr << "Clipmap morph band is invalid.\n";
            return 1;
        }

        if (!NearlyEqual(
                orbit::terrain_view::LodMorphFactor(
                    level,
                    level.morphStartHalfExtentMeters),
                0.0,
                1.0e-12) ||
            !NearlyEqual(
                orbit::terrain_view::LodMorphFactor(
                    level,
                    level.morphEndHalfExtentMeters),
                1.0,
                1.0e-12))
        {
            std::cerr << "Clipmap morph factor endpoints failed.\n";
            return 1;
        }

        if (index > 0)
        {
            const auto& finer = layout.levels[index - 1U];

            if (level.innerHoleHalfExtentMeters >
                finer.outerHalfExtentMeters)
            {
                std::cerr << "Clipmap levels contain a coverage gap.\n";
                return 1;
            }

            if (!NearlyEqual(
                    level.innerHoleHalfExtentMeters,
                    finer.morphStartHalfExtentMeters,
                    1.0e-9))
            {
                std::cerr
                    << "Coarse clipmap ring does not begin at the finer morph band.\n";
                return 1;
            }

            if (!(level.outerHalfExtentMeters >
                  finer.outerHalfExtentMeters))
            {
                std::cerr << "Clipmap extent did not grow with LOD.\n";
                return 1;
            }
        }
    }

    // The fine and coarse centers are independently snapped, so their relative
    // phase can be half a coarse cell on either axis. A coarse cell may be
    // removed only when the complete cell lies inside the finer guaranteed
    // coverage. This is the coverage-first invariant used by the working Godot
    // terrain handoff and prevents rectangular holes at clipmap boundaries.
    {
        const auto& coarse = layout.levels[1];
        const orbit::f64 spacing =
            coarse.sampleSpacingMeters;
        const orbit::f64 halfCell =
            spacing * 0.5;
        const orbit::u32 cellsPerAxis =
            coarse.gridResolution - 1U;

        bool exercisedPartialCell = false;

        // Include a deliberately off-phase quarter parent cell here to
        // regression-test the conservative coverage predicate itself. Runtime
        // clipmap centers are phase-locked below, so this is an adversarial
        // geometry test rather than an expected steady-state phase.
        for (const orbit::f64 phaseX :
             {-halfCell * 0.5, 0.0, halfCell * 0.5})
        {
            for (const orbit::f64 phaseY :
                 {-halfCell * 0.5, 0.0, halfCell * 0.5})
            {
                for (orbit::u32 y = 0;
                     y < cellsPerAxis;
                     ++y)
                {
                    for (orbit::u32 x = 0;
                         x < cellsPerAxis;
                         ++x)
                    {
                        const orbit::f64 centerX =
                            (static_cast<orbit::f64>(x) +
                             0.5 -
                             static_cast<orbit::f64>(
                                 cellsPerAxis) *
                                 0.5) *
                            spacing;

                        const orbit::f64 centerY =
                            (static_cast<orbit::f64>(y) +
                             0.5 -
                             static_cast<orbit::f64>(
                                 cellsPerAxis) *
                                 0.5) *
                            spacing;

                        const bool fullyCovered =
                            orbit::terrain_view::
                                ClipmapCellFullyInsideInnerHole(
                                    coarse,
                                    centerX,
                                    centerY,
                                    phaseX,
                                    phaseY);

                        const bool centerInside =
                            std::abs(
                                centerX -
                                phaseX) <
                                coarse.
                                    innerHoleHalfExtentMeters &&
                            std::abs(
                                centerY -
                                phaseY) <
                                coarse.
                                    innerHoleHalfExtentMeters;

                        if (centerInside &&
                            !fullyCovered)
                        {
                            exercisedPartialCell = true;
                        }

                        if (!fullyCovered)
                        {
                            continue;
                        }

                        const orbit::f64 farX =
                            std::abs(
                                centerX -
                                phaseX) +
                            halfCell;

                        const orbit::f64 farY =
                            std::abs(
                                centerY -
                                phaseY) +
                            halfCell;

                        if (farX >
                                coarse.
                                    innerHoleHalfExtentMeters +
                                    1.0e-9 ||
                            farY >
                                coarse.
                                    innerHoleHalfExtentMeters +
                                    1.0e-9)
                        {
                            std::cerr
                                << "Clipmap hole removed a coarse cell not fully covered by the finer level.\n";
                            return 1;
                        }
                    }
                }
            }
        }

        if (!exercisedPartialCell)
        {
            std::cerr
                << "Clipmap coverage regression did not exercise a partial-cell boundary.\n";
            return 1;
        }
    }

    const orbit::f64 baseOuterExtent =
        orbit::terrain_view::
            ClipmapOuterHalfExtentMeters(
                config);

    const auto tierTwoConfig =
        orbit::terrain_view::
            ClipmapConfigForTier(
                config,
                2);

    if (!NearlyEqual(
            tierTwoConfig.baseSpacingMeters,
            config.baseSpacingMeters * 4.0,
            1.0e-12) ||
        !NearlyEqual(
            orbit::terrain_view::
                ClipmapOuterHalfExtentMeters(
                    tierTwoConfig),
            baseOuterExtent * 4.0,
            1.0e-6))
    {
        std::cerr
            << "Adaptive clipmap tier scaling is incorrect.\n";
        return 1;
    }

    const orbit::terrain_view::
        AdaptiveClipmapCoverageConfig
        adaptiveCoverage{
            .enabled = true,
            .altitudeToHalfExtentScale = 1.0,
            .growThreshold = 0.85,
            .shrinkThreshold = 0.65,
            .maximumTier = 3
        };

    const orbit::u32 grownTier =
        orbit::terrain_view::
            SelectAdaptiveClipmapTier(
                config,
                adaptiveCoverage,
                baseOuterExtent * 0.90,
                0);

    if (grownTier != 1)
    {
        std::cerr
            << "Adaptive clipmap coverage did not grow before exhausting the active tier.\n";
        return 1;
    }

    const orbit::u32 heldTier =
        orbit::terrain_view::
            SelectAdaptiveClipmapTier(
                config,
                adaptiveCoverage,
                baseOuterExtent * 0.70,
                grownTier);

    if (heldTier != 1)
    {
        std::cerr
            << "Adaptive clipmap coverage hysteresis did not hold the current tier.\n";
        return 1;
    }

    const orbit::u32 shrunkTier =
        orbit::terrain_view::
            SelectAdaptiveClipmapTier(
                config,
                adaptiveCoverage,
                baseOuterExtent * 0.50,
                heldTier);

    if (shrunkTier != 0)
    {
        std::cerr
            << "Adaptive clipmap coverage did not shrink after crossing the hysteresis band.\n";
        return 1;
    }

    auto disabledCoverage =
        adaptiveCoverage;

    disabledCoverage.enabled = false;

    if (orbit::terrain_view::
            SelectAdaptiveClipmapTier(
                config,
                disabledCoverage,
                baseOuterExtent * 100.0,
                3) != 0)
    {
        std::cerr
            << "Disabled adaptive clipmap coverage did not return the base tier.\n";
        return 1;
    }

    orbit::terrain_view::ClipmapTracker tracker(
        planet,
        config);

    const auto initial = tracker.Update(observer);

    for (const auto& level : initial.levels)
    {
        if (!level.fullRefresh ||
            level.cellShiftX != 0 ||
            level.cellShiftY != 0)
        {
            std::cerr << "Initial clipmap fill is invalid.\n";
            return 1;
        }
    }

    const orbit::world::SurfaceFrame startFrame =
        initial.levels.front().surfaceFrame;

    const auto makeObserver =
        [&planet, &startFrame](const orbit::f64 eastMeters)
        {
            const orbit::math::Double3 direction =
                orbit::world::DirectionAtSurfaceOffset(
                    planet,
                    startFrame,
                    {eastMeters, 0.0});

            return orbit::world::WorldPosition{
                .meters =
                    direction * (planet.radiusMeters + 1'000.0)
            };
        };

    // L0 is phase-locked to L1, so with a 2:1 LOD ratio its center moves
    // in two-L0-cell increments. This keeps every L0 transition boundary on
    // an L1 grid coordinate instead of alternating into a half-cell phase.
    const auto subCell = tracker.Update(makeObserver(99.0));

    if (subCell.levels.front().cellShiftX != 0 ||
        subCell.levels.front().cellShiftY != 0)
    {
        std::cerr << "Sub-alignment observer motion moved the clipmap.\n";
        return 1;
    }

    const auto oneCell = tracker.Update(makeObserver(101.0));

    if (oneCell.levels[0].cellShiftX != 2 ||
        oneCell.levels[0].cellShiftY != 0 ||
        oneCell.levels[0].fullRefresh)
    {
        std::cerr
            << "Phase-locked spherical lattice did not reuse a two-cell toroidal shift.\n";
        return 1;
    }

    if (oneCell.levels[1].cellShiftX != 0 ||
        oneCell.levels[1].fullRefresh)
    {
        std::cerr << "Coarser clipmap moved or refreshed too early.\n";
        return 1;
    }

    // Every LOD must retain exactly the same anchor frame between rare global
    // rebases. Independent per-level frame rotation is what previously made
    // retained toroidal samples acquire a different world-space address.
    for (orbit::u32 index = 0;
         index < config.levelCount;
         ++index)
    {
        if (orbit::math::Length(
                oneCell.levels[index].surfaceFrame.east -
                startFrame.east) >
                1.0e-12 ||
            orbit::math::Length(
                oneCell.levels[index].surfaceFrame.north -
                startFrame.north) >
                1.0e-12 ||
            orbit::math::Length(
                oneCell.levels[index].surfaceFrame.up -
                startFrame.up) >
                1.0e-12)
        {
            std::cerr
                << "Clipmap LODs did not retain the shared stable lattice frame.\n";
            return 1;
        }
    }

    const orbit::math::Double2 observerFromAnchor =
        orbit::world::SurfaceOffsetBetweenDirections(
            planet,
            oneCell.levels[0].surfaceFrame,
            orbit::math::Normalize(makeObserver(101.0).meters));

    const orbit::math::Double2 observerFromSnappedCenter{
        observerFromAnchor.x -
            oneCell.levels[0].centerOffsetMeters.x,
        observerFromAnchor.y -
            oneCell.levels[0].centerOffsetMeters.y
    };

    if (std::abs(observerFromSnappedCenter.x) > 100.0 + 1.0e-6 ||
        std::abs(observerFromSnappedCenter.y) > 100.0 + 1.0e-6)
    {
        std::cerr << "Clipmap snap did not keep observer within half a cell.\n";
        return 1;
    }

    // A retained physical sample changes logical index when the toroidal
    // origin advances, but centerOffset advances by the exact opposite world
    // distance. Its absolute lattice coordinate must therefore be unchanged.
    const orbit::math::Double2 retainedOldOffset{3'200.0, 2'700.0};
    const orbit::math::Double3 retainedOldDirection =
        orbit::world::DirectionAtSurfaceOffset(
            planet,
            startFrame,
            retainedOldOffset);

    const orbit::math::Double2 retainedNewAbsoluteOffset{
        oneCell.levels[0].centerOffsetMeters.x +
            retainedOldOffset.x -
            static_cast<orbit::f64>(
                oneCell.levels[0].cellShiftX) *
                config.baseSpacingMeters,
        oneCell.levels[0].centerOffsetMeters.y +
            retainedOldOffset.y
    };

    const orbit::math::Double3 retainedAfterRecenter =
        orbit::world::DirectionAtSurfaceOffset(
            planet,
            oneCell.levels[0].surfaceFrame,
            retainedNewAbsoluteOffset);

    const orbit::f64 retainedDriftMeters =
        std::acos(
            std::clamp(
                orbit::math::Dot(
                    retainedOldDirection,
                    retainedAfterRecenter),
                -1.0,
                1.0)) *
        planet.radiusMeters;

    if (retainedDriftMeters > 1.0e-5)
    {
        std::cerr
            << "Stable spherical lattice changed a retained toroidal sample address.\n";
        return 1;
    }

    const auto multiCell = tracker.Update(makeObserver(451.0));

    if (std::abs(multiCell.levels[0].cellShiftX) != 2 ||
        multiCell.levels[0].fullRefresh)
    {
        std::cerr
            << "Multi-cell clipmap jump did not stay on the toroidal fast path.\n";
        return 1;
    }

    // Every fine center must remain on its parent's lattice. This is the
    // invariant that makes the fine morph edge and coarse hole use the same
    // phase for every observer position.
    for (orbit::u32 index = 0;
         index + 1U < config.levelCount;
         ++index)
    {
        const orbit::f64 parentSpacing =
            layout.levels[index + 1U].
                sampleSpacingMeters;

        const orbit::f64 phaseX =
            multiCell.levels[index].
                centerOffsetMeters.x /
            parentSpacing;

        const orbit::f64 phaseY =
            multiCell.levels[index].
                centerOffsetMeters.y /
            parentSpacing;

        if (std::abs(
                phaseX -
                std::round(phaseX)) >
                1.0e-9 ||
            std::abs(
                phaseY -
                std::round(phaseY)) >
                1.0e-9)
        {
            std::cerr
                << "Clipmap child center lost parent-grid phase alignment.\n";
            return 1;
        }
    }

    const auto rebased =
        tracker.Update(
            makeObserver(
                planet.radiusMeters * 0.026));

    for (const auto& level : rebased.levels)
    {
        if (!level.fullRefresh ||
            level.cellShiftX != 0 ||
            level.cellShiftY != 0)
        {
            std::cerr
                << "Long-distance spherical lattice rebase did not refresh coherently.\n";
            return 1;
        }
    }

    tracker.Reset();
    const auto reset = tracker.Update(observer);

    if (!reset.levels.front().fullRefresh)
    {
        std::cerr << "Clipmap reset did not force a full refresh.\n";
        return 1;
    }

    return 0;
}
