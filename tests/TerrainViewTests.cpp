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

            if (!(level.outerHalfExtentMeters >
                  finer.outerHalfExtentMeters))
            {
                std::cerr << "Clipmap extent did not grow with LOD.\n";
                return 1;
            }
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

    const auto subCell = tracker.Update(makeObserver(49.0));

    if (subCell.levels.front().cellShiftX != 0 ||
        subCell.levels.front().cellShiftY != 0)
    {
        std::cerr << "Sub-cell observer motion moved the clipmap.\n";
        return 1;
    }

    const auto oneCell = tracker.Update(makeObserver(51.0));

    if (oneCell.levels[0].cellShiftX != 1 ||
        oneCell.levels[0].cellShiftY != 0 ||
        oneCell.levels[0].fullRefresh)
    {
        std::cerr << "Fine clipmap did not snap by one cell.\n";
        return 1;
    }

    if (oneCell.levels[1].cellShiftX != 0)
    {
        std::cerr << "Coarser clipmap moved too early.\n";
        return 1;
    }

    const orbit::math::Double2 observerFromSnappedCenter =
        orbit::world::SurfaceOffsetBetweenDirections(
            planet,
            oneCell.levels[0].surfaceFrame,
            orbit::math::Normalize(makeObserver(51.0).meters));

    if (std::abs(observerFromSnappedCenter.x) > 50.0 + 1.0e-6)
    {
        std::cerr << "Clipmap snap did not keep observer within half a cell.\n";
        return 1;
    }

    const auto multiCell = tracker.Update(makeObserver(451.0));

    if (std::abs(multiCell.levels[0].cellShiftX) < 3)
    {
        std::cerr << "Multi-cell clipmap jump was not detected.\n";
        return 1;
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
