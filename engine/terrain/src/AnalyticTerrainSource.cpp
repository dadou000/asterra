#include <orbit/terrain/AnalyticTerrainSource.hpp>

#include "ProceduralNoise.hpp"

#include <algorithm>

namespace orbit::terrain
{
namespace
{
[[nodiscard]] GlobalTerrainFieldDesc
ResolveGlobalDesc(
    const AnalyticTerrainDesc& desc) noexcept
{
    GlobalTerrainFieldDesc global =
        desc.global;

    if (global.seed == 0)
    {
        global.seed =
            desc.seed ^
            0xA57E22A6C0A57E22ULL;
    }

    return global;
}
} // namespace

AnalyticTerrainSource::AnalyticTerrainSource(
    const world::PlanetDefinition planet,
    const AnalyticTerrainDesc desc)
    : planet_(planet),
      desc_(desc),
      globalFields_(
          planet,
          ResolveGlobalDesc(desc))
{
}

TerrainSample AnalyticTerrainSource::Sample(
    const TerrainQuery& query) const noexcept
{
    const math::Double3 direction =
        math::Normalize(
            query.unitDirection);

    if (math::LengthSquared(
            direction) <= 0.0)
    {
        return {};
    }

    const GlobalTerrainFieldSample global =
        globalFields_.Sample(query);

    f64 elevation =
        global.coarseElevationMeters;

    const f64 macroWeight =
        detail::DetailWeight(
            desc_.macroWavelengthMeters,
            query.footprintMeters);

    elevation +=
        detail::SampleBand(
            direction,
            planet_.radiusMeters,
            desc_.macroWavelengthMeters,
            desc_.seed ^
                0x632BE59BD9B4E019ULL) *
        desc_.macroAmplitudeMeters *
        macroWeight;

    f64 wavelength =
        desc_.detailWavelengthMeters;

    f64 amplitude =
        desc_.detailAmplitudeMeters;

    for (u32 octave = 0;
         octave < desc_.detailOctaves;
         ++octave)
    {
        const f64 weight =
            detail::DetailWeight(
                wavelength,
                query.footprintMeters);

        if (weight <= 0.0)
        {
            break;
        }

        elevation +=
            detail::SampleBand(
                direction,
                planet_.radiusMeters,
                wavelength,
                desc_.seed +
                    static_cast<u64>(
                        octave + 1U) *
                    0x9E3779B97F4A7C15ULL) *
            amplitude *
            weight;

        wavelength *= 0.5;
        amplitude *= 0.5;
    }

    return {
        .elevationMeters =
            elevation,
        .coarseElevationMeters =
            global.coarseElevationMeters,
        .climate =
            global.climate,
        .biomes =
            ClassifyBiomeWeights(
                global.climate,
                elevation,
                globalFields_.
                    Description().
                    seaLevelMeters)
    };
}
} // namespace orbit::terrain
