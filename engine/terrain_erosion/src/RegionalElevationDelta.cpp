#include <orbit/terrain_erosion/RegionalElevationDelta.hpp>

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
    const f64 spacingMeters,
    const RegionalElevationDeltaConfig& config) noexcept
{
    if (footprintMeters <= 0.0 ||
        spacingMeters <= 0.0)
    {
        return 1.0;
    }

    const f64 fullDetail =
        spacingMeters *
        config.
            fullDetailFootprintScale;

    const f64 fadeOut =
        spacingMeters *
        config.
            fadeOutFootprintScale;

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

    const f64 normalized =
        (footprintMeters -
         fullDetail) /
        std::max(
            fadeOut -
                fullDetail,
            1.0e-6);

    return
        1.0 -
        SmoothStep01(
            normalized);
}
} // namespace

f32 RegionalElevationDeltaField::At(
    const u32 x,
    const u32 y) const
{
    if (x >= resolution ||
        y >= resolution)
    {
        throw std::out_of_range(
            "Orbit regional elevation delta coordinate is out of range.");
    }

    return elevationDeltaMeters[
        static_cast<std::size_t>(y) *
            resolution +
        x];
}

f64 RegionalElevationDeltaField::SampleOffset(
    const math::Double2& offsetMeters) const noexcept
{
    if (resolution < 2 ||
        spacingMeters <= 0.0 ||
        elevationDeltaMeters.size() !=
            static_cast<std::size_t>(
                resolution) *
            resolution ||
        std::abs(
            offsetMeters.x) >
            halfExtentMeters ||
        std::abs(
            offsetMeters.y) >
            halfExtentMeters)
    {
        return 0.0;
    }

    const f64 sampleMaximum =
        static_cast<f64>(
            resolution - 1U);

    const f64 sampleX =
        std::clamp(
            (offsetMeters.x +
             halfExtentMeters) /
                spacingMeters,
            0.0,
            sampleMaximum);

    const f64 sampleY =
        std::clamp(
            (offsetMeters.y +
             halfExtentMeters) /
                spacingMeters,
            0.0,
            sampleMaximum);

    const u32 x0 =
        static_cast<u32>(
            std::floor(
                sampleX));

    const u32 y0 =
        static_cast<u32>(
            std::floor(
                sampleY));

    const u32 x1 =
        std::min(
            x0 + 1U,
            resolution - 1U);

    const u32 y1 =
        std::min(
            y0 + 1U,
            resolution - 1U);

    const f64 tx =
        sampleX -
        static_cast<f64>(
            x0);

    const f64 ty =
        sampleY -
        static_cast<f64>(
            y0);

    const auto sample =
        [this](
            const u32 x,
            const u32 y) noexcept
        {
            return static_cast<f64>(
                elevationDeltaMeters[
                    static_cast<std::size_t>(
                        y) *
                        resolution +
                    x]);
        };

    const f64 top =
        sample(
            x0,
            y0) *
            (1.0 - tx) +
        sample(
            x1,
            y0) *
            tx;

    const f64 bottom =
        sample(
            x0,
            y1) *
            (1.0 - tx) +
        sample(
            x1,
            y1) *
            tx;

    return
        top *
            (1.0 - ty) +
        bottom *
            ty;
}

RegionalElevationDeltaField
BuildRegionalElevationDeltaField(
    const terrain_hydrology::HydrologyGrid& hydrology,
    const std::span<const f32>
        cumulativeElevationDeltaMeters)
{
    const u32 resolution =
        hydrology.config.resolution;

    if (resolution < 2 ||
        hydrology.spacingMeters <=
            0.0 ||
        cumulativeElevationDeltaMeters.size() !=
            hydrology.cells.size() ||
        hydrology.cells.size() !=
            static_cast<std::size_t>(
                resolution) *
            resolution)
    {
        throw std::invalid_argument(
            "Orbit regional elevation delta requires a matching populated hydrology grid.");
    }

    RegionalElevationDeltaField field{};

    field.surfaceFrame =
        hydrology.surfaceFrame;

    field.resolution =
        resolution;

    field.halfExtentMeters =
        hydrology.config.
            halfExtentMeters;

    field.spacingMeters =
        hydrology.spacingMeters;

    field.elevationDeltaMeters.assign(
        cumulativeElevationDeltaMeters.
            begin(),
        cumulativeElevationDeltaMeters.
            end());

    return field;
}

RegionalElevationDeltaTerrainSource::
RegionalElevationDeltaTerrainSource(
    const world::PlanetDefinition planet,
    std::shared_ptr<const terrain::TerrainSource> source,
    std::vector<RegionalElevationDeltaField> fields,
    const RegionalElevationDeltaConfig config)
    : planet_(planet),
      source_(std::move(source)),
      fields_(std::move(fields)),
      config_(config)
{
    if (!source_)
    {
        throw std::invalid_argument(
            "Orbit regional elevation delta terrain requires an underlying source.");
    }

    if (planet_.radiusMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Orbit regional elevation delta terrain requires a positive planet radius.");
    }

    if (config_.regionEdgeFadeMeters <
            0.0 ||
        config_.fullDetailFootprintScale <
            0.0 ||
        config_.fadeOutFootprintScale <=
            config_.
                fullDetailFootprintScale)
    {
        throw std::invalid_argument(
            "Orbit regional elevation delta configuration is invalid.");
    }

    localRevision_ =
        0x61E9D4C2A7835BF1ULL ^
        static_cast<u64>(
            fields_.size());

    for (const RegionalElevationDeltaField& field :
         fields_)
    {
        localRevision_ ^=
            static_cast<u64>(
                field.resolution) *
            0x9E3779B97F4A7C15ULL;

        localRevision_ ^=
            static_cast<u64>(
                field.
                    elevationDeltaMeters.
                    size()) *
            0xBF58476D1CE4E5B9ULL;
    }
}

terrain::TerrainSample
RegionalElevationDeltaTerrainSource::Sample(
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

    f64 elevation =
        result.elevationMeters;

    for (const RegionalElevationDeltaField& field :
         fields_)
    {
        const math::Double2 offset =
            world::SurfaceOffsetBetweenDirections(
                planet_,
                field.surfaceFrame,
                direction);

        const f64 maximumOffset =
            std::max(
                std::abs(
                    offset.x),
                std::abs(
                    offset.y));

        if (maximumOffset >
            field.halfExtentMeters)
        {
            continue;
        }

        const f64 delta =
            field.SampleOffset(
                offset);

        if (delta == 0.0)
        {
            continue;
        }

        const f64 edgeDistance =
            field.halfExtentMeters -
            maximumOffset;

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
                field.spacingMeters,
                config_);

        elevation +=
            delta *
            edgeWeight *
            lodWeight;
    }

    result.elevationMeters =
        elevation;

    return result;
}

u64 RegionalElevationDeltaTerrainSource::Revision()
    const noexcept
{
    return
        source_->Revision() ^
        localRevision_;
}

const std::vector<
    RegionalElevationDeltaField>&
RegionalElevationDeltaTerrainSource::Fields()
    const noexcept
{
    return fields_;
}
} // namespace orbit::terrain_erosion
