#include <orbit/terrain_region/DerivedRegionTerrainSource.hpp>

#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainFields.hpp>
#include <orbit/terrain_erosion/RiverCarving.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace orbit::terrain_region
{
namespace
{
[[nodiscard]] f64 SmoothStep01(
    const f64 value) noexcept
{
    const f64 t =
        std::clamp(
            value,
            0.0,
            1.0);

    return
        t * t *
        (3.0 - 2.0 * t);
}

[[nodiscard]] f64 RegionInfluence(
    const DerivedTerrainRegion& region,
    const math::Double2& offset) noexcept
{
    const f64 maximumOffset =
        std::max(
            std::abs(offset.x),
            std::abs(offset.y));

    if (maximumOffset >=
        region.halfExtentMeters)
    {
        return 0.0;
    }

    const f64 coreHalfExtent =
        region.approximateTileWidthMeters *
        0.5;

    if (maximumOffset <=
        coreHalfExtent)
    {
        return 1.0;
    }

    const f64 overlapWidth =
        region.halfExtentMeters -
        coreHalfExtent;

    if (overlapWidth <=
        1.0e-6)
    {
        return 1.0;
    }

    return
        1.0 -
        SmoothStep01(
            (maximumOffset -
             coreHalfExtent) /
            overlapWidth);
}

[[nodiscard]] f64 FootprintWeight(
    const f64 footprintMeters,
    const f64 spacingMeters,
    const DerivedRegionTerrainSourceConfig& config) noexcept
{
    if (footprintMeters <= 0.0 ||
        spacingMeters <= 0.0)
    {
        return 1.0;
    }

    const f64 fullDetail =
        spacingMeters *
        config.fullDetailFootprintScale;

    const f64 fadeOut =
        spacingMeters *
        config.fadeOutFootprintScale;

    if (footprintMeters <=
        fullDetail)
    {
        return 1.0;
    }

    if (footprintMeters >=
        fadeOut)
    {
        return 0.0;
    }

    return
        1.0 -
        SmoothStep01(
            (footprintMeters -
             fullDetail) /
            std::max(
                fadeOut -
                    fullDetail,
                1.0e-6));
}

[[nodiscard]] u64 MixRevision(
    const u64 a,
    const u64 b) noexcept
{
    u64 value =
        a ^
        (b +
         0x9E3779B97F4A7C15ULL +
         (a << 6U) +
         (a >> 2U));

    value ^= value >> 30U;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27U;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31U;

    return value;
}

[[nodiscard]] terrain::PlanetSurfacePosition
RegionOrigin(
    const world::PlanetDefinition& planet,
    const terrain::PlanetSurfacePosition& storedOrigin,
    const world::SurfaceFrame& frame) noexcept
{
    if (storedOrigin.planet.IsValid())
    {
        return terrain::CanonicalizeSurfacePosition(
            storedOrigin);
    }

    return terrain::CanonicalizeSurfacePosition({
        .planet = planet.id,
        .unitDirection = frame.up
    });
}
} // namespace

DerivedRegionTerrainSource::
DerivedRegionTerrainSource(
    const world::PlanetDefinition planet,
    std::shared_ptr<const terrain::TerrainSource> source,
    std::shared_ptr<DerivedTerrainRegionCache> regionCache,
    std::shared_ptr<DerivedTerrainRegionCache> fineRegionCache,
    const DerivedRegionTerrainSourceConfig config)
    : planet_(planet),
      source_(std::move(source)),
      regionCache_(std::move(regionCache)),
      fineRegionCache_(std::move(fineRegionCache)),
      config_(config)
{
    if (!source_ ||
        !regionCache_)
    {
        throw std::invalid_argument(
            "Orbit derived region terrain source requires a base source and region cache.");
    }

    if (planet_.radiusMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Orbit derived region terrain source requires a positive planet radius.");
    }

    if (config_.fadeOutFootprintScale <=
            config_.fullDetailFootprintScale ||
        config_.fullDetailFootprintScale <
            0.0 ||
        config_.maximumWetlandBlend <
            0.0F ||
        config_.maximumWetlandBlend >
            1.0F)
    {
        throw std::invalid_argument(
            "Orbit derived region terrain source configuration is invalid.");
    }
}

DerivedRegionTerrainSource::
DerivedRegionTerrainSource(
    const world::PlanetDefinition planet,
    std::shared_ptr<const terrain::TerrainSource> source,
    std::shared_ptr<DerivedTerrainRegionCache> regionCache,
    const DerivedRegionTerrainSourceConfig config)
    : DerivedRegionTerrainSource(
        planet,
        std::move(source),
        std::move(regionCache),
        nullptr,
        config)
{
}

bool DerivedRegionTerrainSource::
AccumulateFromCache(
    const DerivedTerrainRegionCache& cache,
    const terrain::PlanetSurfacePosition& position,
    const terrain::TerrainSampleFootprint& footprint,
    const f64 baseElevation,
    f64& totalWeight,
    f64& weightedRegionalDelta,
    f64& weightedCarveDelta,
    f64& weightedWetlandInfluence)
    const noexcept
{
    const u64 sourceRevision =
        source_->Revision();

    const auto regions =
        cache.ReadyRegionsSnapshot();

    if (!regions ||
        regions->empty())
    {
        return false;
    }

    for (const auto& region :
         *regions)
    {
        if (!region ||
            region->id.sourceRevision !=
                sourceRevision)
        {
            continue;
        }

        const terrain::PlanetSurfacePosition origin =
            RegionOrigin(
                planet_,
                region->elevationDelta.origin,
                region->elevationDelta.surfaceFrame);

        const math::Double2 offset =
            terrain::SurfaceOffsetBetweenPositions(
                planet_,
                origin,
                region->elevationDelta.surfaceFrame,
                position);

        if (!std::isfinite(offset.x) ||
            !std::isfinite(offset.y))
        {
            continue;
        }

        const f64 regionWeight =
            RegionInfluence(
                *region,
                offset);

        if (regionWeight <= 0.0)
        {
            continue;
        }

        totalWeight +=
            regionWeight;

        const f64 lodWeight =
            FootprintWeight(
                footprint.diameterMeters,
                region->
                    elevationDelta.
                    spacingMeters,
                config_);

        if (lodWeight <= 0.0)
        {
            continue;
        }

        const f64 localRegionalDelta =
            region->
                elevationDelta.
                SampleOffset(
                    offset);

        const f64 weightedInfluence =
            regionWeight *
            lodWeight;

        weightedRegionalDelta +=
            localRegionalDelta *
            weightedInfluence;

        const auto carving =
            terrain_erosion::
                SampleRiverCarving(
                    region->carving,
                    offset);

        if (!carving.active)
        {
            continue;
        }

        const f64 localShapedElevation =
            baseElevation +
            localRegionalDelta;

        const f64 localCarveDelta =
            std::min(
                carving.targetElevationMeters -
                    localShapedElevation,
                0.0);

        weightedCarveDelta +=
            localCarveDelta *
            weightedInfluence;

        weightedWetlandInfluence +=
            carving.influence *
            weightedInfluence;
    }

    return totalWeight > 0.0;
}

terrain::TerrainSample
DerivedRegionTerrainSource::Sample(
    const terrain::TerrainQuery& query) const noexcept
{
    const f64 directionLengthSquared =
        math::LengthSquared(
            query.unitDirection);

    if (!std::isfinite(query.unitDirection.x) ||
        !std::isfinite(query.unitDirection.y) ||
        !std::isfinite(query.unitDirection.z) ||
        !std::isfinite(directionLengthSquared) ||
        directionLengthSquared <= 0.0)
    {
        return source_->Sample(query);
    }

    if (query.planet.IsValid() &&
        planet_.id.IsValid() &&
        query.planet != planet_.id)
    {
        return source_->Sample(query);
    }

    const terrain::PlanetSurfacePosition position =
        terrain::CanonicalizeSurfacePosition({
            .planet = query.planet.IsValid()
                ? query.planet
                : planet_.id,
            .unitDirection = query.unitDirection,
            .radialOffsetMeters =
                query.radialOffsetMeters
        });

    const terrain::TerrainSampleFootprint footprint =
        query.Footprint();

    const terrain::TerrainQuery canonicalQuery =
        terrain::MakeTerrainQuery(
            position,
            footprint);

    terrain::TerrainSample result =
        source_->Sample(
            canonicalQuery);

    const f64 baseElevation =
        result.elevationMeters;

    f64 totalWeight = 0.0;
    f64 weightedRegionalDelta = 0.0;
    f64 weightedCarveDelta = 0.0;
    f64 weightedWetlandInfluence = 0.0;

    // The fine cache, where it has ready coverage, fully replaces
    // the coarse one rather than blending with it -- averaging a
    // finely-carved near-camera delta with a coarse one would just
    // produce a third, still-wrong shape. Only fall back to the
    // coarse cache where the fine one has nothing loaded (e.g. just
    // outside its streamed neighborhood).
    const bool usedFine =
        fineRegionCache_ &&
        AccumulateFromCache(
            *fineRegionCache_,
            position,
            footprint,
            baseElevation,
            totalWeight,
            weightedRegionalDelta,
            weightedCarveDelta,
            weightedWetlandInfluence);

    if (!usedFine)
    {
        totalWeight = 0.0;
        weightedRegionalDelta = 0.0;
        weightedCarveDelta = 0.0;
        weightedWetlandInfluence = 0.0;

        if (!AccumulateFromCache(
                *regionCache_,
                position,
                footprint,
                baseElevation,
                totalWeight,
                weightedRegionalDelta,
                weightedCarveDelta,
                weightedWetlandInfluence))
        {
            return result;
        }
    }

    const f64 inverseTotalWeight =
        1.0 /
        totalWeight;

    const f64 regionalDelta =
        weightedRegionalDelta *
        inverseTotalWeight;

    const f64 carveDelta =
        weightedCarveDelta *
        inverseTotalWeight;

    const f64 wetlandInfluence =
        weightedWetlandInfluence *
        inverseTotalWeight;

    result.elevationMeters =
        baseElevation +
        regionalDelta +
        carveDelta;

    // Standing basins have one authority (the coarse hydrology cache).
    // A fine region arriving refines rivers/land, but cannot replace a lake's
    // spill height with an independently solved local depression.
    f64 lakeRegionWeight = 0.0;
    f64 lakeInfluence = 0.0;
    f64 lakeBed = 0.0;
    f64 lakeDepth = 0.0;
    const auto lakeRegions = regionCache_->ReadyRegionsSnapshot();
    if (lakeRegions)
    {
        for (const auto& region : *lakeRegions)
        {
            if (!region || region->id.sourceRevision != source_->Revision())
            {
                continue;
            }

            const terrain::PlanetSurfacePosition lakeOrigin =
                RegionOrigin(
                    planet_,
                    region->elevationDelta.origin,
                    region->lakes.surfaceFrame);

            const auto offset =
                terrain::SurfaceOffsetBetweenPositions(
                    planet_,
                    lakeOrigin,
                    region->lakes.surfaceFrame,
                    position);

            if (!std::isfinite(offset.x) ||
                !std::isfinite(offset.y))
            {
                continue;
            }

            const f64 regionWeight = RegionInfluence(*region, offset);
            if (regionWeight <= 0.0)
            {
                continue;
            }
            lakeRegionWeight += regionWeight;
            const f64 weight = regionWeight * FootprintWeight(
                footprint.diameterMeters, region->lakes.cellSpacingMeters, config_);
            if (weight <= 0.0)
            {
                continue;
            }
            const auto lake = terrain_water::SampleLakeWater(region->lakes, offset);
            lakeInfluence += weight * lake.influence;
            lakeBed += weight * lake.influence * lake.bedElevationMeters;
            lakeDepth += weight * lake.depthMeters;
        }
    }
    const f64 originalWaterSurface = baseElevation + result.standingWaterDepthMeters;
    const bool hadOcean = result.standingWaterDepthMeters > 0.0;
    if (lakeRegionWeight > 0.0)
    {
        result.elevationMeters = result.elevationMeters *
            (1.0 - std::clamp(lakeInfluence / lakeRegionWeight, 0.0, 1.0)) +
            lakeBed / lakeRegionWeight;
        result.standingWaterDepthMeters = lakeDepth / lakeRegionWeight;
    }
    if (hadOcean)
    {
        result.standingWaterDepthMeters = std::max(result.standingWaterDepthMeters,
            std::max(originalWaterSurface - result.elevationMeters, 0.0));
    }

    if (wetlandInfluence > 0.0 &&
        result.biomes.ocean < 0.5F)
    {
        const f32 wetlandBlend =
            static_cast<f32>(
                std::clamp(
                    wetlandInfluence,
                    0.0,
                    1.0)) *
            config_.maximumWetlandBlend;

        result.biomes.wetland +=
            wetlandBlend;

        result.biomes =
            terrain::NormalizeBiomeWeights(
                result.biomes);
    }

    return result;
}

u64 DerivedRegionTerrainSource::Revision()
    const noexcept
{
    const u64 mixed =
        MixRevision(
            source_->Revision(),
            regionCache_->
                ContentRevision());

    if (!fineRegionCache_)
    {
        return mixed;
    }

    return
        MixRevision(
            mixed,
            fineRegionCache_->
                ContentRevision());
}
} // namespace orbit::terrain_region