#include <orbit/math/Vector.hpp>
#include <orbit/terrain_view/ClipmapLayout.hpp>
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
        orbit::math::Normalize({1.0, 0.2, 1.0});

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

    const orbit::math::Double3 offsetDirection =
        orbit::world::DirectionAtSurfaceOffset(
            planet,
            frame,
            {eastOffset, 0.0});

    const orbit::f64 angularDistance = std::acos(
        std::clamp(
            orbit::math::Dot(frame.up, offsetDirection),
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

    const orbit::world::WorldPosition observer{
        .meters = seamDirection * (planet.radiusMeters + 1'000.0)
    };

    const orbit::terrain_view::ClipmapConfig config{
        .levelCount = 8,
        .gridResolution = 129,
        .baseSpacingMeters = 2.0,
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

    return 0;
}
