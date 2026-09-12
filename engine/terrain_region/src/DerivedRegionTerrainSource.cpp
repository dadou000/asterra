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

} // namespace

DerivedRegionTerrainSource::
DerivedRegionTerrainSource(
    const world::PlanetDefinition planet,
    std::shared_ptr<const terrain::TerrainSource> source,
    std::shared_ptr<DerivedTerrainRegionCache> regionCache,
    const DerivedRegionTerrainSourceConfig config)
    : planet_(planet),
      source_(std::move(source)),
      regionCache_(std::move(regionCache)),
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

terrain::TerrainSample
DerivedRegionTerrainSource::Sample(
    const terrain::TerrainQuery& query) const noexcept
{
    terrain::TerrainSample result =
        source_->Sample(query);

    const math::Double3 direction =
        math::Normalize(
            query.unitDirection);

    if (math::LengthSquared(direction) <=
        0.0)
    {
        return result;
    }

    const u64 sourceRevision =
        source_->Revision();

    const auto regions =
        regionCache_->
            ReadyRegionsSnapshot();

    if (!regions ||
        regions->empty())
    {
        return result;
    }

    const f64 baseElevation =
        result.elevationMeters;

    f64 totalWeight = 0.0;
    f64 weightedRegionalDelta = 0.0;
    f64 weightedCarveDelta = 0.0;
    f64 weightedWetlandInfluence = 0.0;

    for (const auto& region :
         *regions)
    {
        if (!region ||
            region->id.sourceRevision !=
                sourceRevision)
        {
            continue;
        }

        const math::Double2 offset =
            world::SurfaceOffsetBetweenDirections(
                planet_,
                region->elevationDelta.surfaceFrame,
                direction);

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
                query.footprintMeters,
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

    if (totalWeight <= 0.0)
    {
        return result;
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
    return
        MixRevision(
            source_->Revision(),
            regionCache_->
                ContentRevision());
}
} // namespace orbit::terrain_region
