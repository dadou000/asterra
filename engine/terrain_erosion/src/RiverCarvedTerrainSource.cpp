#include <orbit/terrain_erosion/RiverCarvedTerrainSource.hpp>

#include <orbit/math/Vector.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace orbit::terrain_erosion
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

[[nodiscard]] f64 FootprintWeight(
    const f64 footprintMeters,
    const f64 valleyHalfWidthMeters,
    const RiverCarvedTerrainConfig& config) noexcept
{
    if (footprintMeters <= 0.0 ||
        valleyHalfWidthMeters <= 0.0)
    {
        return 1.0;
    }

    const f64 diameter =
        valleyHalfWidthMeters *
        2.0;

    const f64 fullDetailFootprint =
        diameter *
        config.
            fullDetailFootprintRatio;

    const f64 fadeOutFootprint =
        diameter *
        config.
            fadeOutFootprintRatio;

    if (footprintMeters <=
        fullDetailFootprint)
    {
        return 1.0;
    }

    if (footprintMeters >=
        fadeOutFootprint)
    {
        return 0.0;
    }

    const f64 normalized =
        (footprintMeters -
         fullDetailFootprint) /
        std::max(
            fadeOutFootprint -
                fullDetailFootprint,
            1.0e-6);

    return
        1.0 -
        SmoothStep01(
            normalized);
}
} // namespace

RiverCarvedTerrainSource::
RiverCarvedTerrainSource(
    const world::PlanetDefinition planet,
    std::shared_ptr<const terrain::TerrainSource> source,
    std::vector<RiverCarvingField> fields,
    const RiverCarvedTerrainConfig config)
    : planet_(planet),
      source_(std::move(source)),
      fields_(std::move(fields)),
      config_(config)
{
    if (!source_)
    {
        throw std::invalid_argument(
            "Orbit river-carved terrain requires an underlying terrain source.");
    }

    if (planet_.radiusMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Orbit river-carved terrain requires a positive planet radius.");
    }

    if (config_.fullDetailFootprintRatio <
            0.0 ||
        config_.fadeOutFootprintRatio <=
            config_.
                fullDetailFootprintRatio ||
        config_.regionEdgeFadeMeters <
            0.0 ||
        config_.maximumWetlandBlend <
            0.0F ||
        config_.maximumWetlandBlend >
            1.0F)
    {
        throw std::invalid_argument(
            "Orbit river-carved terrain configuration is invalid.");
    }

    localRevision_ =
        0xE20D14A5C47B91F3ULL ^
        static_cast<u64>(
            fields_.size());

    for (const RiverCarvingField& field :
         fields_)
    {
        localRevision_ ^=
            static_cast<u64>(
                field.nodes.size()) *
            0x9E3779B97F4A7C15ULL;

        localRevision_ ^=
            static_cast<u64>(
                field.segments.size()) *
            0xBF58476D1CE4E5B9ULL;
    }
}

terrain::TerrainSample
RiverCarvedTerrainSource::Sample(
    const terrain::TerrainQuery& query) const noexcept
{
    terrain::TerrainSample result =
        source_->Sample(query);

    const math::Double3 direction =
        math::Normalize(
            query.unitDirection);

    if (math::LengthSquared(
            direction) <= 0.0)
    {
        return result;
    }

    f64 finalElevation =
        result.elevationMeters;

    f64 strongestWetlandInfluence =
        0.0;

    for (const RiverCarvingField& field :
         fields_)
    {
        const math::Double2 offset =
            world::SurfaceOffsetBetweenDirections(
                planet_,
                field.surfaceFrame,
                direction);

        const RiverCarvingSample carving =
            SampleRiverCarving(
                field,
                offset);

        if (!carving.active)
        {
            continue;
        }

        const f64 edgeDistance =
            field.halfExtentMeters -
            std::max(
                std::abs(
                    offset.x),
                std::abs(
                    offset.y));

        const f64 edgeWeight =
            config_.regionEdgeFadeMeters >
                    0.0
                ? SmoothStep01(
                    edgeDistance /
                    config_.
                        regionEdgeFadeMeters)
                : 1.0;

        const f64 lodWeight =
            FootprintWeight(
                query.footprintMeters,
                carving.
                    valleyHalfWidthMeters,
                config_) *
            edgeWeight;

        if (lodWeight <= 0.0)
        {
            continue;
        }

        const f64 target =
            std::min(
                finalElevation,
                carving.
                    targetElevationMeters);

        finalElevation =
            finalElevation +
            (target -
             finalElevation) *
                lodWeight;

        strongestWetlandInfluence =
            std::max(
                strongestWetlandInfluence,
                carving.influence *
                    lodWeight);
    }

    result.elevationMeters =
        finalElevation;

    if (strongestWetlandInfluence >
            0.0 &&
        result.biomes.ocean <
            0.5F)
    {
        const f32 wetlandBlend =
            static_cast<f32>(
                std::clamp(
                    strongestWetlandInfluence,
                    0.0,
                    1.0)) *
            config_.
                maximumWetlandBlend;

        result.biomes.wetland +=
            wetlandBlend;

        result.biomes =
            terrain::NormalizeBiomeWeights(
                result.biomes);
    }

    return result;
}

u64 RiverCarvedTerrainSource::Revision()
    const noexcept
{
    return
        source_->Revision() ^
        localRevision_;
}

const std::vector<RiverCarvingField>&
RiverCarvedTerrainSource::Fields()
    const noexcept
{
    return fields_;
}
} // namespace orbit::terrain_erosion
