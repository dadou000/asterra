#include <orbit/terrain_region/DerivedTerrainRegion.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
class DeterministicTerrainSource final :
    public orbit::terrain::TerrainSource
{
public:
    explicit DeterministicTerrainSource(
        const orbit::u64 revision,
        const bool mutateDuringSampling = false) noexcept :
        revision_(revision),
        mutateDuringSampling_(mutateDuringSampling)
    {
    }

    [[nodiscard]] orbit::terrain::TerrainSample Sample(
        const orbit::terrain::TerrainQuery& query)
        const noexcept override
    {
        ++sampleCount_;

        if (mutateDuringSampling_ &&
            sampleCount_ == 1)
        {
            ++revision_;
        }

        const orbit::f64 elevation =
            1'000.0 +
            query.unitDirection.x * 160.0 +
            query.unitDirection.y * 90.0 +
            query.unitDirection.z * 45.0;

        return {
            .elevationMeters = elevation,
            .coarseElevationMeters = elevation,
            .climate = {
                .temperatureC = 18.0F,
                .humidity = 0.6F,
                .precipitation = 0.8F,
                .continentality = 0.4F
            },
            .biomes = {
                .grassland = 1.0F
            }
        };
    }

    [[nodiscard]] orbit::u64 Revision()
        const noexcept override
    {
        return revision_;
    }

private:
    mutable orbit::u64 revision_{0};
    mutable orbit::u64 sampleCount_{0};
    bool mutateDuringSampling_{false};
};

[[nodiscard]] bool SameHydrologyCell(
    const orbit::terrain_hydrology::HydrologyCell& a,
    const orbit::terrain_hydrology::HydrologyCell& b)
{
    return
        a.elevationMeters == b.elevationMeters &&
        a.drainageElevationMeters ==
            b.drainageElevationMeters &&
        a.depressionFillMeters ==
            b.depressionFillMeters &&
        a.runoffWeight == b.runoffWeight &&
        a.flowAccumulation ==
            b.flowAccumulation &&
        a.oceanWeight == b.oceanWeight &&
        a.flowDx == b.flowDx &&
        a.flowDy == b.flowDy;
}

[[nodiscard]] bool SameRegion(
    const orbit::terrain_region::DerivedTerrainRegion& a,
    const orbit::terrain_region::DerivedTerrainRegion& b)
{
    if (!(a.id == b.id) ||
        a.approximateTileWidthMeters !=
            b.approximateTileWidthMeters ||
        a.halfExtentMeters !=
            b.halfExtentMeters ||
        a.hydrology.spacingMeters !=
            b.hydrology.spacingMeters ||
        a.hydrology.cells.size() !=
            b.hydrology.cells.size() ||
        a.elevationDelta.elevationDeltaMeters !=
            b.elevationDelta.elevationDeltaMeters ||
        a.rivers.nodes.size() !=
            b.rivers.nodes.size() ||
        a.rivers.segments.size() !=
            b.rivers.segments.size() ||
        a.carving.nodes.size() !=
            b.carving.nodes.size() ||
        a.carving.segments.size() !=
            b.carving.segments.size() ||
        a.carving.spatialCellOffsets !=
            b.carving.spatialCellOffsets ||
        a.carving.spatialSegmentIndices !=
            b.carving.spatialSegmentIndices)
    {
        return false;
    }

    if (!std::equal(
            a.hydrology.cells.begin(),
            a.hydrology.cells.end(),
            b.hydrology.cells.begin(),
            SameHydrologyCell))
    {
        return false;
    }

    for (std::size_t index = 0;
         index < a.rivers.nodes.size();
         ++index)
    {
        const auto& left =
            a.rivers.nodes[index];
        const auto& right =
            b.rivers.nodes[index];

        if (left.sourceCellIndex !=
                right.sourceCellIndex ||
            left.offsetMeters.x !=
                right.offsetMeters.x ||
            left.offsetMeters.y !=
                right.offsetMeters.y ||
            left.elevationMeters !=
                right.elevationMeters ||
            left.drainageElevationMeters !=
                right.drainageElevationMeters ||
            left.depressionFillMeters !=
                right.depressionFillMeters ||
            left.drainageAreaSquareMeters !=
                right.drainageAreaSquareMeters ||
            left.oceanWeight !=
                right.oceanWeight)
        {
            return false;
        }
    }

    for (std::size_t index = 0;
         index < a.rivers.segments.size();
         ++index)
    {
        const auto& left =
            a.rivers.segments[index];
        const auto& right =
            b.rivers.segments[index];

        if (left.upstreamNode !=
                right.upstreamNode ||
            left.downstreamNode !=
                right.downstreamNode)
        {
            return false;
        }
    }

    for (std::size_t index = 0;
         index < a.carving.nodes.size();
         ++index)
    {
        const auto& left =
            a.carving.nodes[index];
        const auto& right =
            b.carving.nodes[index];

        if (left.sourceRiverNode !=
                right.sourceRiverNode ||
            left.offsetMeters.x !=
                right.offsetMeters.x ||
            left.offsetMeters.y !=
                right.offsetMeters.y ||
            left.sourceElevationMeters !=
                right.sourceElevationMeters ||
            left.drainageElevationMeters !=
                right.drainageElevationMeters ||
            left.bedElevationMeters !=
                right.bedElevationMeters ||
            left.channelHalfWidthMeters !=
                right.channelHalfWidthMeters ||
            left.valleyHalfWidthMeters !=
                right.valleyHalfWidthMeters ||
            left.drainageAreaSquareMeters !=
                right.drainageAreaSquareMeters)
        {
            return false;
        }
    }

    for (std::size_t index = 0;
         index < a.carving.segments.size();
         ++index)
    {
        const auto& left =
            a.carving.segments[index];
        const auto& right =
            b.carving.segments[index];

        if (left.upstreamNode !=
                right.upstreamNode ||
            left.downstreamNode !=
                right.downstreamNode)
        {
            return false;
        }
    }

    return
        a.carving.spatialResolution ==
            b.carving.spatialResolution &&
        a.carving.spatialHalfExtentMeters ==
            b.carving.spatialHalfExtentMeters &&
        a.carving.spatialCellSizeMeters ==
            b.carving.spatialCellSizeMeters;
}

[[nodiscard]] orbit::terrain_region::DerivedTerrainRegionConfig
TestConfig()
{
    orbit::terrain_region::DerivedTerrainRegionConfig
        config{};

    config.generatorVersion = 3;
    config.overlapScale = 1.10;
    config.hydrology.resolution = 9;
    config.hydrology.footprintMeters = 0.0;
    config.hydrology.useCoarseElevation = false;
    config.hydrology.conditionDepressions = true;
    config.hydrology.minimumDrainageDropMeters =
        0.01;
    config.refinement.iterations = 1;
    config.refinement.elevationDeltaScale =
        0.20;
    config.minimumRiverDrainageAreaSquareMeters =
        1.0;
    config.carving.spatialIndexResolution = 8;

    return config;
}
} // namespace

int main()
{
    const orbit::world::PlanetDefinition planet{
        .radiusMeters = 6'000'000.0
    };

    const orbit::terrain_region::DerivedTerrainRegionId
        id{
            .tile = {
                .face =
                    orbit::world::CubeFace::PositiveZ,
                .level = 8,
                .x = 100,
                .y = 120
            },
            .sourceRevision = 7,
            .generatorVersion = 3
        };

    const auto config = TestConfig();

    DeterministicTerrainSource source(7);

    const auto first =
        orbit::terrain_region::
            BuildDerivedTerrainRegion(
                planet,
                source,
                id,
                config);

    const auto second =
        orbit::terrain_region::
            BuildDerivedTerrainRegion(
                planet,
                source,
                id,
                config);

    if (!SameRegion(
            first,
            second))
    {
        std::cerr
            << "Derived terrain region generation is not deterministic.\n";
        return 1;
    }

    const orbit::f64 expectedHalfExtent =
        first.approximateTileWidthMeters *
        config.overlapScale *
        0.5;

    if (std::abs(
            first.halfExtentMeters -
            expectedHalfExtent) >
        1.0e-9)
    {
        std::cerr
            << "Derived terrain region overlap extent is incorrect.\n";
        return 1;
    }

    auto otherVersion = id;
    ++otherVersion.generatorVersion;

    if (otherVersion == id)
    {
        std::cerr
            << "Derived terrain region generator version is not part of identity.\n";
        return 1;
    }

    const orbit::terrain_region::
        DerivedTerrainRegionIdHash hasher;

    if (hasher(otherVersion) ==
        hasher(id))
    {
        std::cerr
            << "Derived terrain region hash ignored generator version.\n";
        return 1;
    }

    bool rejectedGeneratorMismatch = false;

    try
    {
        static_cast<void>(
            orbit::terrain_region::
                BuildDerivedTerrainRegion(
                    planet,
                    source,
                    otherVersion,
                    config));
    }
    catch (const std::invalid_argument&)
    {
        rejectedGeneratorMismatch = true;
    }

    if (!rejectedGeneratorMismatch)
    {
        std::cerr
            << "Derived terrain region accepted a mismatched generator version.\n";
        return 1;
    }

    bool rejectedStaleBeforeBuild = false;

    try
    {
        DeterministicTerrainSource staleSource(8);

        static_cast<void>(
            orbit::terrain_region::
                BuildDerivedTerrainRegion(
                    planet,
                    staleSource,
                    id,
                    config));
    }
    catch (const std::runtime_error&)
    {
        rejectedStaleBeforeBuild = true;
    }

    if (!rejectedStaleBeforeBuild)
    {
        std::cerr
            << "Derived terrain region accepted an already-stale source revision.\n";
        return 1;
    }

    bool rejectedRevisionDrift = false;

    try
    {
        DeterministicTerrainSource mutatingSource(
            7,
            true);

        static_cast<void>(
            orbit::terrain_region::
                BuildDerivedTerrainRegion(
                    planet,
                    mutatingSource,
                    id,
                    config));
    }
    catch (const std::runtime_error&)
    {
        rejectedRevisionDrift = true;
    }

    if (!rejectedRevisionDrift)
    {
        std::cerr
            << "Derived terrain region accepted a source revision that changed during generation.\n";
        return 1;
    }

    return 0;
}
