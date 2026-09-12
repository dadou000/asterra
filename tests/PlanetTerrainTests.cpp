#include <orbit/math/Vector.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/world/Planet.hpp>

#include <array>
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

bool DirectionNearlyEqual(
    const orbit::math::Double3& a,
    const orbit::math::Double3& b,
    const orbit::f64 epsilon)
{
    const auto delta = a - b;
    return orbit::math::LengthSquared(delta) <= epsilon * epsilon;
}
} // namespace

int main()
{
    using orbit::math::Double3;
    using orbit::world::CubeCoordinate;
    using orbit::world::CubeFace;

    constexpr std::array<Double3, 10> directions{{
        {1.0, 0.0, 0.0},
        {-1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0},
        {0.0, -1.0, 0.0},
        {0.0, 0.0, 1.0},
        {0.0, 0.0, -1.0},
        {1.0, 1.0, 0.999999},
        {1.0, -0.999999, 1.0},
        {-0.75, 0.33, 0.58},
        {0.12, -0.98, 0.41}
    }};

    for (const Double3& rawDirection : directions)
    {
        const Double3 direction = orbit::math::Normalize(rawDirection);
        const CubeCoordinate cube =
            orbit::world::UnitDirectionToCube(direction);
        const Double3 reconstructed =
            orbit::world::CubeToUnitDirection(cube);

        if (!DirectionNearlyEqual(
                direction,
                reconstructed,
                1.0e-12))
        {
            std::cerr << "Cube-sphere round trip failed.\n";
            return 1;
        }
    }

    const auto positiveXTile =
        orbit::world::TileForDirection({1.0, 0.0, 0.0}, 4);

    if (positiveXTile.face != CubeFace::PositiveX ||
        positiveXTile.x != 8 ||
        positiveXTile.y != 8)
    {
        std::cerr << "Planet tile addressing failed.\n";
        return 1;
    }

    const orbit::world::PlanetDefinition planet{
        .radiusMeters = 6'000'000.0
    };

    const auto level4 = orbit::world::PlanetTileId{
        .face = CubeFace::PositiveZ,
        .level = 4,
        .x = 8,
        .y = 8
    };

    const auto level5 = orbit::world::PlanetTileId{
        .face = CubeFace::PositiveZ,
        .level = 5,
        .x = 16,
        .y = 16
    };

    const orbit::f64 width4 =
        orbit::world::ApproximateTileWidthMeters(planet, level4);
    const orbit::f64 width5 =
        orbit::world::ApproximateTileWidthMeters(planet, level5);

    if (!(width5 < width4) ||
        !NearlyEqual(width5 / width4, 0.5, 0.03))
    {
        std::cerr << "Planet quadtree scale progression failed.\n";
        return 1;
    }

    const orbit::terrain::AnalyticTerrainSource terrain(
        planet,
        {
            .seed = 12345,
            .macroAmplitudeMeters = 0.0,
            .macroWavelengthMeters = 1'000'000.0,
            .detailAmplitudeMeters = 100.0,
            .detailWavelengthMeters = 1'000.0,
            .detailOctaves = 1
        });

    const Double3 terrainDirection =
        orbit::math::Normalize({0.37, 0.81, -0.45});

    const auto fine = terrain.Sample({
        .unitDirection = terrainDirection,
        .footprintMeters = 10.0
    });

    const auto repeated = terrain.Sample({
        .unitDirection = terrainDirection,
        .footprintMeters = 10.0
    });

    const auto coarse = terrain.Sample({
        .unitDirection = terrainDirection,
        .footprintMeters = 600.0
    });

    if (fine.elevationMeters != repeated.elevationMeters)
    {
        std::cerr << "Terrain sampling is not deterministic.\n";
        return 1;
    }

    if (std::abs(fine.elevationMeters) < 1.0e-9)
    {
        std::cerr << "Fine terrain test sample unexpectedly vanished.\n";
        return 1;
    }

    if (std::abs(coarse.elevationMeters) > 1.0e-12)
    {
        std::cerr << "Terrain footprint filtering failed.\n";
        return 1;
    }

    return 0;
}
