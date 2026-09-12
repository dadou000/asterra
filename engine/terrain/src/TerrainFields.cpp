#include <orbit/terrain/TerrainFields.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::terrain
{
namespace
{
[[nodiscard]] f32 Saturate(
    const f64 value) noexcept
{
    return static_cast<f32>(
        std::clamp(
            value,
            0.0,
            1.0));
}

[[nodiscard]] f64 SmoothStep(
    const f64 edge0,
    const f64 edge1,
    const f64 value) noexcept
{
    if (edge1 <= edge0)
    {
        return value >= edge1
            ? 1.0
            : 0.0;
    }

    const f64 t =
        std::clamp(
            (value - edge0) /
                (edge1 - edge0),
            0.0,
            1.0);

    return t * t *
        (3.0 - 2.0 * t);
}
} // namespace

BiomeWeights NormalizeBiomeWeights(
    const BiomeWeights& weights) noexcept
{
    BiomeWeights result{
        .ocean =
            std::max(
                weights.ocean,
                0.0F),
        .desert =
            std::max(
                weights.desert,
                0.0F),
        .grassland =
            std::max(
                weights.grassland,
                0.0F),
        .temperateForest =
            std::max(
                weights.temperateForest,
                0.0F),
        .borealForest =
            std::max(
                weights.borealForest,
                0.0F),
        .tundra =
            std::max(
                weights.tundra,
                0.0F),
        .alpine =
            std::max(
                weights.alpine,
                0.0F),
        .wetland =
            std::max(
                weights.wetland,
                0.0F)
    };

    const f32 sum =
        result.ocean +
        result.desert +
        result.grassland +
        result.temperateForest +
        result.borealForest +
        result.tundra +
        result.alpine +
        result.wetland;

    if (sum <= 1.0e-6F)
    {
        return {
            .grassland = 1.0F
        };
    }

    const f32 inverse =
        1.0F / sum;

    result.ocean *= inverse;
    result.desert *= inverse;
    result.grassland *= inverse;
    result.temperateForest *= inverse;
    result.borealForest *= inverse;
    result.tundra *= inverse;
    result.alpine *= inverse;
    result.wetland *= inverse;

    return result;
}

BiomeWeights LerpBiomeWeights(
    const BiomeWeights& a,
    const BiomeWeights& b,
    const f64 t) noexcept
{
    const f32 blend =
        Saturate(t);

    const auto lerp =
        [blend](
            const f32 x,
            const f32 y) noexcept
        {
            return x +
                (y - x) *
                blend;
        };

    return NormalizeBiomeWeights({
        .ocean =
            lerp(
                a.ocean,
                b.ocean),
        .desert =
            lerp(
                a.desert,
                b.desert),
        .grassland =
            lerp(
                a.grassland,
                b.grassland),
        .temperateForest =
            lerp(
                a.temperateForest,
                b.temperateForest),
        .borealForest =
            lerp(
                a.borealForest,
                b.borealForest),
        .tundra =
            lerp(
                a.tundra,
                b.tundra),
        .alpine =
            lerp(
                a.alpine,
                b.alpine),
        .wetland =
            lerp(
                a.wetland,
                b.wetland)
    });
}

TerrainClimate LerpTerrainClimate(
    const TerrainClimate& a,
    const TerrainClimate& b,
    const f64 t) noexcept
{
    const f32 blend =
        Saturate(t);

    const auto lerp =
        [blend](
            const f32 x,
            const f32 y) noexcept
        {
            return x +
                (y - x) *
                blend;
        };

    return {
        .temperatureC =
            lerp(
                a.temperatureC,
                b.temperatureC),
        .humidity =
            lerp(
                a.humidity,
                b.humidity),
        .precipitation =
            lerp(
                a.precipitation,
                b.precipitation),
        .continentality =
            lerp(
                a.continentality,
                b.continentality)
    };
}

TerrainSample LerpTerrainSample(
    const TerrainSample& a,
    const TerrainSample& b,
    const f64 t) noexcept
{
    const f64 blend =
        std::clamp(
            t,
            0.0,
            1.0);

    return {
        .elevationMeters =
            a.elevationMeters +
            (b.elevationMeters -
             a.elevationMeters) *
                blend,
        .coarseElevationMeters =
            a.coarseElevationMeters +
            (b.coarseElevationMeters -
             a.coarseElevationMeters) *
                blend,
        .climate =
            LerpTerrainClimate(
                a.climate,
                b.climate,
                blend),
        .biomes =
            LerpBiomeWeights(
                a.biomes,
                b.biomes,
                blend)
    };
}

BiomeWeights ClassifyBiomeWeights(
    const TerrainClimate& climate,
    const f64 elevationMeters,
    const f64 seaLevelMeters) noexcept
{
    const f64 relativeElevation =
        elevationMeters -
        seaLevelMeters;

    const f64 land =
        SmoothStep(
            -80.0,
            120.0,
            relativeElevation);

    const f64 ocean =
        1.0 - land;

    const f64 alpine =
        land *
        SmoothStep(
            1'800.0,
            3'800.0,
            relativeElevation);

    const f64 nonAlpine =
        land *
        (1.0 - alpine);

    const f64 temperature =
        static_cast<f64>(
            climate.temperatureC);

    const f64 precipitation =
        std::clamp(
            static_cast<f64>(
                climate.precipitation),
            0.0,
            1.0);

    const f64 humidity =
        std::clamp(
            static_cast<f64>(
                climate.humidity),
            0.0,
            1.0);

    const f64 hot =
        SmoothStep(
            18.0,
            31.0,
            temperature);

    const f64 cold =
        1.0 -
        SmoothStep(
            -8.0,
            8.0,
            temperature);

    const f64 cool =
        (1.0 - cold) *
        (1.0 -
         SmoothStep(
             10.0,
             19.0,
             temperature));

    const f64 temperate =
        SmoothStep(
            4.0,
            14.0,
            temperature) *
        (1.0 -
         SmoothStep(
             23.0,
             31.0,
             temperature));

    const f64 dry =
        1.0 -
        SmoothStep(
            0.22,
            0.48,
            precipitation);

    const f64 moist =
        SmoothStep(
            0.35,
            0.68,
            precipitation);

    const f64 saturated =
        SmoothStep(
            0.72,
            0.94,
            humidity) *
        SmoothStep(
            0.62,
            0.88,
            precipitation);

    const f64 lowland =
        1.0 -
        SmoothStep(
            350.0,
            1'100.0,
            std::abs(
                relativeElevation));

    return NormalizeBiomeWeights({
        .ocean =
            static_cast<f32>(
                ocean * 3.0),
        .desert =
            static_cast<f32>(
                nonAlpine *
                hot *
                dry *
                1.7),
        .grassland =
            static_cast<f32>(
                nonAlpine *
                (0.35 +
                 temperate) *
                (1.0 -
                 0.65 * moist) *
                (1.0 -
                 0.7 * dry)),
        .temperateForest =
            static_cast<f32>(
                nonAlpine *
                temperate *
                moist *
                1.5),
        .borealForest =
            static_cast<f32>(
                nonAlpine *
                cool *
                moist *
                1.35),
        .tundra =
            static_cast<f32>(
                nonAlpine *
                cold *
                (0.5 +
                 0.5 *
                 (1.0 - dry)) *
                1.5),
        .alpine =
            static_cast<f32>(
                alpine * 2.2),
        .wetland =
            static_cast<f32>(
                nonAlpine *
                saturated *
                lowland *
                (1.0 - cold) *
                1.4)
    });
}
} // namespace orbit::terrain
