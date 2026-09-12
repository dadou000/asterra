#include <orbit/terrain_erosion/RegionalElevationDelta.hpp>
#include <orbit/world/Planet.hpp>

#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
class FlatTerrainSource final :
    public orbit::terrain::TerrainSource
{
public:
    [[nodiscard]] orbit::terrain::TerrainSample
    Sample(
        const orbit::terrain::TerrainQuery&)
        const noexcept override
    {
        return {
            .elevationMeters = 100.0,
            .coarseElevationMeters = 100.0,
            .biomes = {
                .grassland = 1.0F
            }
        };
    }
};

bool NearlyEqual(
    const orbit::f64 a,
    const orbit::f64 b,
    const orbit::f64 epsilon = 1.0e-5)
{
    return
        std::abs(a - b) <=
        epsilon;
}
} // namespace

int main()
{
    const orbit::world::PlanetDefinition planet{
        .radiusMeters = 6'000'000.0
    };

    orbit::terrain_hydrology::HydrologyGrid
        hydrology{};

    hydrology.config = {
        .resolution = 3,
        .halfExtentMeters = 1'000.0
    };

    hydrology.spacingMeters =
        1'000.0;

    hydrology.surfaceFrame =
        orbit::world::MakeSurfaceFrame({
            1.0,
            0.0,
            0.0
        });

    hydrology.cells.resize(9);

    const std::vector<orbit::f32> deltas{
        0.0F,  0.0F,   0.0F,
        0.0F, -10.0F, -20.0F,
        0.0F,  0.0F,   0.0F
    };

    const auto field =
        orbit::terrain_erosion::
            BuildRegionalElevationDeltaField(
                hydrology,
                deltas);

    if (!NearlyEqual(
            field.SampleOffset({
                0.0,
                0.0
            }),
            -10.0))
    {
        std::cerr
            << "Regional elevation delta center sample is wrong.\n";
        return 1;
    }

    if (!NearlyEqual(
            field.SampleOffset({
                500.0,
                0.0
            }),
            -15.0))
    {
        std::cerr
            << "Regional elevation delta bilinear interpolation is wrong.\n";
        return 1;
    }

    auto flatSource =
        std::make_shared<
            FlatTerrainSource>();

    orbit::terrain_erosion::
        RegionalElevationDeltaTerrainSource
            terrain(
                planet,
                flatSource,
                {field},
                {
                    .regionEdgeFadeMeters =
                        100.0,
                    .fullDetailFootprintScale =
                        0.5,
                    .fadeOutFootprintScale =
                        4.0
                });

    const auto center =
        terrain.Sample({
            .unitDirection =
                hydrology.
                    surfaceFrame.up,
            .footprintMeters =
                1.0
        });

    if (!NearlyEqual(
            center.elevationMeters,
            90.0))
    {
        std::cerr
            << "Regional elevation delta terrain did not apply the center delta.\n";
        return 1;
    }

    const auto coarse =
        terrain.Sample({
            .unitDirection =
                hydrology.
                    surfaceFrame.up,
            .footprintMeters =
                10'000.0
        });

    if (!NearlyEqual(
            coarse.elevationMeters,
            100.0))
    {
        std::cerr
            << "Regional elevation delta did not fade out for coarse footprints.\n";
        return 1;
    }

    const orbit::math::Double3
        edgeDirection =
            orbit::world::
                DirectionAtSurfaceOffset(
                    planet,
                    hydrology.
                        surfaceFrame,
                    {
                        1'000.0,
                        0.0
                    });

    const auto edge =
        terrain.Sample({
            .unitDirection =
                edgeDirection,
            .footprintMeters =
                1.0
        });

    if (!NearlyEqual(
            edge.elevationMeters,
            100.0,
            1.0e-3))
    {
        std::cerr
            << "Regional elevation delta did not fade to zero at the region boundary.\n";
        return 1;
    }

    return 0;
}
