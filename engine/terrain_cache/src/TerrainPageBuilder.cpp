#include <orbit/terrain_cache/TerrainPageBuilder.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace orbit::terrain_cache
{
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
    return hash;
}

f32 TerrainPage::At(
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

    return elevationMeters[index];
}

f32 TerrainPage::SampleDirection(
    const math::Double3& unitDirection) const noexcept
{
    if (desc.resolution < 2 ||
        elevationMeters.size() !=
            static_cast<std::size_t>(
                desc.resolution) *
            static_cast<std::size_t>(
                desc.resolution))
    {
        return 0.0F;
    }

    const world::CubeCoordinate cube =
        world::UnitDirectionToCube(
            unitDirection);

    if (cube.face != desc.tile.face)
    {
        return 0.0F;
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
        return 0.0F;
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

    const std::size_t resolution =
        static_cast<std::size_t>(
            desc.resolution);

    const auto sample =
        [this, resolution](
            const u32 x,
            const u32 y) noexcept
        {
            return elevationMeters[
                static_cast<std::size_t>(y) *
                    resolution +
                x];
        };

    const f64 top =
        static_cast<f64>(
            sample(x0, y0)) *
            (1.0 - tx) +
        static_cast<f64>(
            sample(x1, y0)) *
            tx;

    const f64 bottom =
        static_cast<f64>(
            sample(x0, y1)) *
            (1.0 - tx) +
        static_cast<f64>(
            sample(x1, y1)) *
            tx;

    return static_cast<f32>(
        top * (1.0 - ty) +
        bottom * ty);
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
        resolution * resolution;

    TerrainPage page{};
    page.desc = desc;
    page.approximateSampleSpacingMeters =
        world::ApproximateTileWidthMeters(
            planet,
            desc.tile) /
        static_cast<f64>(
            desc.resolution - 1U);

    page.elevationMeters.resize(
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

            const terrain::TerrainSample
                sampleValue =
                    source.Sample({
                        .unitDirection =
                            direction,
                        .footprintMeters =
                            page.
                                approximateSampleSpacingMeters
                    });

            const std::size_t index =
                static_cast<std::size_t>(y) *
                    resolution +
                static_cast<std::size_t>(x);

            page.elevationMeters[index] =
                static_cast<f32>(
                    sampleValue.
                        elevationMeters);
        }
    }

    return page;
}
} // namespace orbit::terrain_cache
