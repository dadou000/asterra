#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_erosion/HydrologyRefinement.hpp>
#include <orbit/terrain_erosion/RegionalElevationDelta.hpp>
#include <orbit/terrain_erosion/RiverCarving.hpp>
#include <orbit/terrain_hydrology/HydrologyGrid.hpp>
#include <orbit/terrain_hydrology/RiverGraph.hpp>
#include <orbit/terrain_water/RiverWater.hpp>
#include <orbit/world/Planet.hpp>

#include <cstddef>

namespace orbit::terrain_region
{
struct DerivedTerrainRegionId
{
    world::PlanetTileId tile{};
    u64 sourceRevision{0};
    u32 generatorVersion{0};

    [[nodiscard]] constexpr bool operator==(
        const DerivedTerrainRegionId&) const noexcept = default;
};

struct DerivedTerrainRegionIdHash
{
    [[nodiscard]] std::size_t operator()(
        const DerivedTerrainRegionId& id) const noexcept;
};

struct DerivedTerrainRegionConfig
{
    u32 generatorVersion{1};
    f64 overlapScale{1.35};

    terrain_hydrology::HydrologyGridConfig hydrology{
        .resolution = 129,
        .halfExtentMeters = 250'000.0,
        .footprintMeters = 0.0,
        .useCoarseElevation = false,
        .conditionDepressions = true,
        .minimumDrainageDropMeters = 0.25
    };

    terrain_erosion::HydrologyRefinementConfig refinement{
        .iterations = 2,
        .elevationDeltaScale = 0.35
    };

    f64 minimumRiverDrainageAreaSquareMeters{
        400'000'000.0
    };

    terrain_erosion::RiverCarvingConfig carving{
        .referenceDrainageAreaSquareMeters =
            1'000'000'000.0,
        .baseChannelHalfWidthMeters = 20.0,
        .minimumChannelHalfWidthMeters = 5.0,
        .maximumChannelHalfWidthMeters = 180.0,
        .widthExponent = 0.30,
        .baseDepthMeters = 8.0,
        .minimumDepthMeters = 2.0,
        .maximumDepthMeters = 80.0,
        .depthExponent = 0.18,
        .valleyWidthMultiplier = 8.0,
        .minimumBedSlope = 0.00008,
        .maximumIncisionMeters = 250.0,
        .spatialIndexResolution = 64
    };

    terrain_water::RiverWaterConfig water{};
};

struct DerivedTerrainRegion
{
    DerivedTerrainRegionId id{};

    f64 approximateTileWidthMeters{0.0};
    f64 halfExtentMeters{0.0};

    terrain_hydrology::HydrologyGrid hydrology{};
    terrain_erosion::RegionalElevationDeltaField elevationDelta{};
    terrain_hydrology::RiverGraph rivers{};
    terrain_erosion::RiverCarvingField carving{};
    terrain_water::RiverWaterNetwork water{};
};

[[nodiscard]] DerivedTerrainRegion BuildDerivedTerrainRegion(
    const world::PlanetDefinition& planet,
    const terrain::TerrainSource& source,
    const DerivedTerrainRegionId& id,
    const DerivedTerrainRegionConfig& config = {});
} // namespace orbit::terrain_region
