#include <orbit/terrain_cache/TerrainPageBuilder.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace orbit::terrain_cache
{
f32 TerrainPage::At(const u32 x, const u32 y) const
{
    if (x >= desc.resolution || y >= desc.resolution)
    {
        throw std::out_of_range(
            "Orbit terrain page sample coordinate is out of range.");
    }

    const std::size_t index =
        static_cast<std::size_t>(y) *
            static_cast<std::size_t>(desc.resolution) +
        static_cast<std::size_t>(x);

    return elevationMeters[index];
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
        static_cast<std::size_t>(desc.resolution);

    if (resolution >
        std::numeric_limits<std::size_t>::max() / resolution)
    {
        throw std::overflow_error(
            "Orbit terrain page sample count overflow.");
    }

    const std::size_t sampleCount = resolution * resolution;

    TerrainPage page{};
    page.desc = desc;
    page.approximateSampleSpacingMeters =
        world::ApproximateTileWidthMeters(
            planet,
            desc.tile) /
        static_cast<f64>(desc.resolution - 1U);

    page.elevationMeters.resize(sampleCount);

    const world::CubeBounds bounds =
        world::TileBounds(desc.tile);

    const f64 denominator =
        static_cast<f64>(desc.resolution - 1U);

    for (u32 y = 0; y < desc.resolution; ++y)
    {
        const f64 ty = static_cast<f64>(y) / denominator;
        const f64 v =
            bounds.minimumUv.y +
            (bounds.maximumUv.y - bounds.minimumUv.y) * ty;

        for (u32 x = 0; x < desc.resolution; ++x)
        {
            const f64 tx = static_cast<f64>(x) / denominator;
            const f64 u =
                bounds.minimumUv.x +
                (bounds.maximumUv.x - bounds.minimumUv.x) * tx;

            const math::Double3 direction =
                world::CubeToUnitDirection({
                    .face = bounds.face,
                    .uv = {u, v}
                });

            const terrain::TerrainSample sample =
                source.Sample({
                    .unitDirection = direction,
                    .footprintMeters =
                        page.approximateSampleSpacingMeters
                });

            const std::size_t index =
                static_cast<std::size_t>(y) * resolution +
                static_cast<std::size_t>(x);

            page.elevationMeters[index] =
                static_cast<f32>(sample.elevationMeters);
        }
    }

    return page;
}
} // namespace orbit::terrain_cache
