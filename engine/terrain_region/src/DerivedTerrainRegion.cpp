#include <orbit/terrain_region/DerivedTerrainRegion.hpp>

#include <cmath>
#include <stdexcept>
#include <utility>

namespace orbit::terrain_region
{
namespace
{
void MixHash(
    u64& hash,
    const u64 value) noexcept
{
    constexpr u64 offsetBasis =
        1'469'598'103'934'665'603ULL;
    constexpr u64 prime =
        1'099'511'628'211ULL;

    if (hash == 0)
    {
        hash = offsetBasis;
    }

    for (u32 byte = 0;
         byte < 8;
         ++byte)
    {
        hash ^=
            (value >> (byte * 8U)) &
            0xFFULL;
        hash *= prime;
    }
}

void RejectStaleSource(
    const terrain::TerrainSource& source,
    const DerivedTerrainRegionId& id,
    const char* stage)
{
    if (source.Revision() !=
        id.sourceRevision)
    {
        throw std::runtime_error(stage);
    }
}
} // namespace

std::size_t DerivedTerrainRegionIdHash::operator()(
    const DerivedTerrainRegionId& id) const noexcept
{
    u64 hash = 0;

    MixHash(
        hash,
        static_cast<u64>(
            id.tile.face));
    MixHash(
        hash,
        static_cast<u64>(
            id.tile.level));
    MixHash(
        hash,
        static_cast<u64>(
            id.tile.x));
    MixHash(
        hash,
        static_cast<u64>(
            id.tile.y));
    MixHash(
        hash,
        id.sourceRevision);
    MixHash(
        hash,
        static_cast<u64>(
            id.generatorVersion));

    return static_cast<std::size_t>(
        hash);
}

DerivedTerrainRegion BuildDerivedTerrainRegion(
    const world::PlanetDefinition& planet,
    const terrain::TerrainSource& source,
    const DerivedTerrainRegionId& id,
    const DerivedTerrainRegionConfig& config)
{
    if (config.generatorVersion == 0 ||
        id.generatorVersion != config.generatorVersion)
    {
        throw std::invalid_argument(
            "Orbit derived terrain region generator version does not match its build configuration.");
    }

    if (!std::isfinite(
            config.overlapScale) ||
        config.overlapScale < 1.0)
    {
        throw std::invalid_argument(
            "Orbit derived terrain regions require a finite overlap scale of at least 1.0.");
    }

    if (!std::isfinite(
            config.
                minimumRiverDrainageAreaSquareMeters) ||
        config.
            minimumRiverDrainageAreaSquareMeters <=
            0.0)
    {
        throw std::invalid_argument(
            "Orbit derived terrain regions require a finite positive river drainage-area threshold.");
    }

    RejectStaleSource(
        source,
        id,
        "Orbit derived terrain region source revision was stale before generation.");

    const math::Double3 centerDirection =
        world::CubeToUnitDirection(
            world::TileCenter(
                id.tile));

    const world::SurfaceFrame surfaceFrame =
        world::MakeSurfaceFrame(
            centerDirection);

    const f64 approximateTileWidthMeters =
        world::ApproximateTileWidthMeters(
            planet,
            id.tile);

    if (!std::isfinite(
            approximateTileWidthMeters) ||
        approximateTileWidthMeters <=
            0.0)
    {
        throw std::invalid_argument(
            "Orbit derived terrain region tile width must be finite and positive.");
    }

    const f64 halfExtentMeters =
        approximateTileWidthMeters *
        config.overlapScale *
        0.5;

    terrain_hydrology::HydrologyGridConfig
        hydrologyConfig =
            config.hydrology;

    hydrologyConfig.halfExtentMeters =
        halfExtentMeters;

    auto hydrology =
        terrain_hydrology::
            BuildHydrologyGrid(
                planet,
                source,
                surfaceFrame,
                hydrologyConfig);

    auto refinement =
        terrain_erosion::
            RefineHydrologyWithSediment(
                std::move(hydrology),
                config.refinement);

    auto rivers =
        terrain_hydrology::
            BuildRiverGraph(
                refinement.hydrology,
                config.
                    minimumRiverDrainageAreaSquareMeters);

    auto elevationDelta =
        terrain_erosion::
            BuildRegionalElevationDeltaField(
                refinement.hydrology,
                refinement.
                    cumulativeElevationDeltaMeters);

    auto carving =
        terrain_erosion::
            BuildRiverCarvingField(
                refinement.hydrology,
                rivers,
                config.carving);

    auto water =
        terrain_water::
            BuildRiverWaterNetwork(
                refinement.hydrology,
                rivers,
                carving,
                approximateTileWidthMeters *
                    0.5,
                config.water);

    auto lakes =
        terrain_water::
            BuildLakeWaterField(
                refinement.hydrology,
                approximateTileWidthMeters *
                    0.5,
                config.lakes);

    RejectStaleSource(
        source,
        id,
        "Orbit derived terrain region source revision changed during generation.");

    return {
        .id = id,
        .approximateTileWidthMeters =
            approximateTileWidthMeters,
        .halfExtentMeters =
            halfExtentMeters,
        .hydrology =
            std::move(
                refinement.hydrology),
        .elevationDelta =
            std::move(
                elevationDelta),
        .rivers =
            std::move(
                rivers),
        .carving =
            std::move(
                carving),
        .water =
            std::move(
                water),
        .lakes =
            std::move(
                lakes)
    };
}
} // namespace orbit::terrain_region
