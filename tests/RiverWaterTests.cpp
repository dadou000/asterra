#include <orbit/terrain_water/RiverWater.hpp>
#include <orbit/world/Planet.hpp>

#include <cmath>
#include <iostream>

int main()
{
    orbit::terrain_hydrology::HydrologyGrid hydrology{};

    hydrology.config = {
        .resolution = 5,
        .halfExtentMeters = 2'000.0
    };

    hydrology.spacingMeters = 1'000.0;

    hydrology.surfaceFrame =
        orbit::world::MakeSurfaceFrame({
            1.0,
            0.0,
            0.0
        });

    orbit::terrain_hydrology::RiverGraph rivers{};

    rivers.nodes = {
        {
            .sourceCellIndex = 0,
            .offsetMeters = {-1'000.0, 0.0},
            .elevationMeters = 120.0F,
            .drainageElevationMeters = 120.0F,
            .drainageAreaSquareMeters =
                250'000'000.0
        },
        {
            .sourceCellIndex = 1,
            .offsetMeters = {0.0, 0.0},
            .elevationMeters = 112.0F,
            .drainageElevationMeters = 112.0F,
            .drainageAreaSquareMeters =
                1'000'000'000.0
        },
        {
            .sourceCellIndex = 2,
            .offsetMeters = {1'000.0, 0.0},
            .elevationMeters = 101.0F,
            .drainageElevationMeters = 101.0F,
            .drainageAreaSquareMeters =
                4'000'000'000.0
        },
        {
            .sourceCellIndex = 3,
            .offsetMeters = {2'500.0, 0.0},
            .elevationMeters = 90.0F,
            .drainageElevationMeters = 90.0F,
            .drainageAreaSquareMeters =
                8'000'000'000.0
        }
    };

    rivers.segments = {
        {.upstreamNode = 0, .downstreamNode = 1},
        {.upstreamNode = 1, .downstreamNode = 2},
        {.upstreamNode = 2, .downstreamNode = 3}
    };

    orbit::terrain_erosion::RiverCarvingField carving{};

    carving.surfaceFrame =
        hydrology.surfaceFrame;

    carving.halfExtentMeters = 3'000.0;

    carving.nodes = {
        {
            .sourceRiverNode = 0,
            .offsetMeters = {-1'000.0, 0.0},
            .sourceElevationMeters = 120.0F,
            .drainageElevationMeters = 120.0F,
            .bedElevationMeters = 116.0F,
            .channelHalfWidthMeters = 8.0F,
            .valleyHalfWidthMeters = 32.0F,
            .drainageAreaSquareMeters =
                250'000'000.0
        },
        {
            .sourceRiverNode = 1,
            .offsetMeters = {0.0, 0.0},
            .sourceElevationMeters = 112.0F,
            .drainageElevationMeters = 112.0F,
            .bedElevationMeters = 106.0F,
            .channelHalfWidthMeters = 14.0F,
            .valleyHalfWidthMeters = 56.0F,
            .drainageAreaSquareMeters =
                1'000'000'000.0
        },
        {
            .sourceRiverNode = 2,
            .offsetMeters = {1'000.0, 0.0},
            .sourceElevationMeters = 101.0F,
            .drainageElevationMeters = 101.0F,
            .bedElevationMeters = 93.0F,
            .channelHalfWidthMeters = 22.0F,
            .valleyHalfWidthMeters = 88.0F,
            .drainageAreaSquareMeters =
                4'000'000'000.0
        },
        {
            .sourceRiverNode = 3,
            .offsetMeters = {2'500.0, 0.0},
            .sourceElevationMeters = 90.0F,
            .drainageElevationMeters = 90.0F,
            .bedElevationMeters = 80.0F,
            .channelHalfWidthMeters = 30.0F,
            .valleyHalfWidthMeters = 120.0F,
            .drainageAreaSquareMeters =
                8'000'000'000.0
        }
    };

    carving.segments = {
        {.upstreamNode = 0, .downstreamNode = 1},
        {.upstreamNode = 1, .downstreamNode = 2},
        {.upstreamNode = 2, .downstreamNode = 3}
    };

    const auto water =
        orbit::terrain_water::BuildRiverWaterNetwork(
            hydrology,
            rivers,
            carving,
            1'500.0,
            {
                .channelFillFraction = 0.80,
                .referenceDrainageAreaSquareMeters =
                    1'000'000'000.0,
                .baseDepthMeters = 1.5,
                .minimumDepthMeters = 0.25,
                .maximumDepthMeters = 8.0,
                .depthExponent = 0.25,
                .bankClearanceMeters = 0.10,
                .manningRoughness = 0.035,
                .minimumSlope = 0.00002,
                .minimumVelocityMetersPerSecond = 0.05,
                .maximumVelocityMetersPerSecond = 8.0
            });

    if (water.segments.size() != 2 ||
        water.nodes.size() != 3)
    {
        std::cerr
            << "River water core ownership did not remove the overlap-only segment.\n";
        return 1;
    }

    if (!(water.nodes[2].halfWidthMeters >
              water.nodes[1].halfWidthMeters &&
          water.nodes[1].halfWidthMeters >
              water.nodes[0].halfWidthMeters))
    {
        std::cerr
            << "River water width did not grow downstream.\n";
        return 1;
    }

    if (!(water.nodes[2].depthMeters >
              water.nodes[1].depthMeters &&
          water.nodes[1].depthMeters >
              water.nodes[0].depthMeters))
    {
        std::cerr
            << "River water depth did not scale with drainage area.\n";
        return 1;
    }

    for (const auto& node : water.nodes)
    {
        if (!(node.surfaceElevationMeters >=
                  node.bedElevationMeters &&
              node.depthMeters >= 0.0F))
        {
            std::cerr
                << "River water node produced an invalid water surface.\n";
            return 1;
        }
    }

    for (const auto& segment : water.segments)
    {
        if (!std::isfinite(
                segment.velocityMetersPerSecond) ||
            segment.velocityMetersPerSecond <= 0.0F ||
            segment.slope <= 0.0F)
        {
            std::cerr
                << "River water flow derivation produced invalid velocity or slope.\n";
            return 1;
        }
    }

    return 0;
}
