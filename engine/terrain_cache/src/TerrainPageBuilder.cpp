#include <orbit/terrain_cache/TerrainPageBuilder.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace orbit::terrain_cache
{
namespace
{
[[nodiscard]] u8 QuantizeWeight(
    const f32 weight) noexcept
{
    return static_cast<u8>(
        std::clamp(
            weight,
            0.0F,
            1.0F) *
            255.0F +
        0.5F);
}

[[nodiscard]] f32 DequantizeWeight(
    const u8 weight) noexcept
{
    return static_cast<f32>(weight) /
           255.0F;
}
} // namespace

CachedTerrainSample ToCachedSample(
    const terrain::TerrainSample& sample)
    noexcept
{
    const terrain::BiomeWeights&
        biomes = sample.biomes;

    return {
        .elevationMeters =
            static_cast<f32>(
                sample.elevationMeters),
        .coarseElevationMeters =
            static_cast<f32>(
                sample.
                    coarseElevationMeters),
        .climate = sample.climate,
        .quantizedBiomeWeights = {
            QuantizeWeight(biomes.ocean),
            QuantizeWeight(biomes.desert),
            QuantizeWeight(
                biomes.grassland),
            QuantizeWeight(
                biomes.temperateForest),
            QuantizeWeight(
                biomes.borealForest),
            QuantizeWeight(biomes.tundra),
            QuantizeWeight(biomes.alpine),
            QuantizeWeight(biomes.wetland)
        },
        .standingWaterDepthMeters = static_cast<f32>(sample.standingWaterDepthMeters)
    };
}

terrain::TerrainSample FromCachedSample(
    const CachedTerrainSample& sample)
    noexcept
{
    const auto& q =
        sample.quantizedBiomeWeights;

    return {
        .elevationMeters =
            static_cast<f64>(
                sample.elevationMeters),
        .coarseElevationMeters =
            static_cast<f64>(
                sample.
                    coarseElevationMeters),
        .climate = sample.climate,
        .biomes = {
            .ocean =
                DequantizeWeight(q[0]),
            .desert =
                DequantizeWeight(q[1]),
            .grassland =
                DequantizeWeight(q[2]),
            .temperateForest =
                DequantizeWeight(q[3]),
            .borealForest =
                DequantizeWeight(q[4]),
            .tundra =
                DequantizeWeight(q[5]),
            .alpine =
                DequantizeWeight(q[6]),
            .wetland =
                DequantizeWeight(q[7])
        },
        .standingWaterDepthMeters = sample.standingWaterDepthMeters
    };
}

std::size_t TerrainPageDescHash::operator()(
    const TerrainPageDesc& desc) const noexcept
{
    std::size_t hash =
        static_cast<std::size_t>(
            desc.tile.face);

    const auto combine =
        [&hash](const u64 value)
        {
            hash ^=
                static_cast<std::size_t>(
                    value) +
                0x9E3779B97F4A7C15ULL +
                (hash << 6U) +
                (hash >> 2U);
        };

    combine(desc.tile.level);
    combine(desc.tile.x);
    combine(desc.tile.y);
    combine(desc.resolution);
    combine(desc.sourceRevision);

    return hash;
}

terrain::TerrainSample
TerrainPage::SampleAt(
    const u32 x,
    const u32 y) const
{
    if (x >= desc.resolution ||
        y >= desc.resolution)
    {
        throw std::out_of_range(
            "Orbit terrain page sample coordinate is out of range.");
    }

    const std::size_t index =
        static_cast<std::size_t>(y) *
            static_cast<std::size_t>(
                desc.resolution) +
        static_cast<std::size_t>(x);

    return FromCachedSample(
        samples[index]);
}

f32 TerrainPage::At(
    const u32 x,
    const u32 y) const
{
    return static_cast<f32>(
        SampleAt(
            x,
            y).
            elevationMeters);
}

terrain::TerrainSample
TerrainPage::SampleDirection(
    const math::Double3& unitDirection) const noexcept
{
    if (desc.resolution < 2 ||
        samples.size() !=
            static_cast<std::size_t>(
                desc.resolution) *
            static_cast<std::size_t>(
                desc.resolution))
    {
        return {};
    }

    const world::CubeCoordinate cube =
        world::UnitDirectionToCube(
            unitDirection);

    if (cube.face != desc.tile.face)
    {
        return {};
    }

    const world::CubeBounds bounds =
        world::TileBounds(
            desc.tile);

    const f64 widthU =
        bounds.maximumUv.x -
        bounds.minimumUv.x;

    const f64 widthV =
        bounds.maximumUv.y -
        bounds.minimumUv.y;

    if (widthU <= 0.0 ||
        widthV <= 0.0)
    {
        return {};
    }

    const f64 sampleMaximum =
        static_cast<f64>(
            desc.resolution - 1U);

    const f64 sampleX =
        std::clamp(
            (cube.uv.x -
             bounds.minimumUv.x) /
                widthU *
                sampleMaximum,
            0.0,
            sampleMaximum);

    const f64 sampleY =
        std::clamp(
            (cube.uv.y -
             bounds.minimumUv.y) /
                widthV *
                sampleMaximum,
            0.0,
            sampleMaximum);

    const u32 x0 =
        static_cast<u32>(
            std::floor(sampleX));

    const u32 y0 =
        static_cast<u32>(
            std::floor(sampleY));

    const u32 x1 =
        std::min(
            x0 + 1U,
            desc.resolution - 1U);

    const u32 y1 =
        std::min(
            y0 + 1U,
            desc.resolution - 1U);

    const f64 tx =
        sampleX -
        static_cast<f64>(x0);

    const f64 ty =
        sampleY -
        static_cast<f64>(y0);

    const terrain::TerrainSample top =
        terrain::LerpTerrainSample(
            FromCachedSample(
                samples[
                    static_cast<
                        std::size_t>(
                        y0) *
                        desc.resolution +
                    x0]),
            FromCachedSample(
                samples[
                    static_cast<
                        std::size_t>(
                        y0) *
                        desc.resolution +
                    x1]),
            tx);

    const terrain::TerrainSample bottom =
        terrain::LerpTerrainSample(
            FromCachedSample(
                samples[
                    static_cast<
                        std::size_t>(
                        y1) *
                        desc.resolution +
                    x0]),
            FromCachedSample(
                samples[
                    static_cast<
                        std::size_t>(
                        y1) *
                        desc.resolution +
                    x1]),
            tx);

    return terrain::LerpTerrainSample(
        top,
        bottom,
        ty);
}

TerrainPage BuildTerrainPage(
    const world::PlanetDefinition& planet,
    const terrain::TerrainSource& source,
    const TerrainPageDesc& desc)
{
    if (desc.resolution < 2)
    {
        throw std::invalid_argument(
            "Orbit terrain pages require at least two samples per axis.");
    }

    const std::size_t resolution =
        static_cast<std::size_t>(
            desc.resolution);

    if (resolution >
        std::numeric_limits<
            std::size_t>::max() /
            resolution)
    {
        throw std::overflow_error(
            "Orbit terrain page sample count overflow.");
    }

    const std::size_t sampleCount =
        resolution *
        resolution;

    TerrainPage page{};
    page.desc = desc;

    page.approximateSampleSpacingMeters =
        world::ApproximateTileWidthMeters(
            planet,
            desc.tile) /
        static_cast<f64>(
            desc.resolution - 1U);

    page.samples.resize(
        sampleCount);

    const world::CubeBounds bounds =
        world::TileBounds(
            desc.tile);

    const f64 denominator =
        static_cast<f64>(
            desc.resolution - 1U);

    for (u32 y = 0;
         y < desc.resolution;
         ++y)
    {
        const f64 ty =
            static_cast<f64>(y) /
            denominator;

        const f64 v =
            bounds.minimumUv.y +
            (bounds.maximumUv.y -
             bounds.minimumUv.y) *
                ty;

        for (u32 x = 0;
             x < desc.resolution;
             ++x)
        {
            const f64 tx =
                static_cast<f64>(x) /
                denominator;

            const f64 u =
                bounds.minimumUv.x +
                (bounds.maximumUv.x -
                 bounds.minimumUv.x) *
                    tx;

            const math::Double3 direction =
                world::CubeToUnitDirection({
                    .face = bounds.face,
                    .uv = {u, v}
                });

            const std::size_t index =
                static_cast<std::size_t>(y) *
                    resolution +
                static_cast<std::size_t>(x);

            page.samples[index] =
                ToCachedSample(
                    source.Sample({
                        .unitDirection =
                            direction,
                        .footprintMeters =
                            page.
                                approximateSampleSpacingMeters
                    }));
        }
    }

    return page;
}
} // namespace orbit::terrain_cache
