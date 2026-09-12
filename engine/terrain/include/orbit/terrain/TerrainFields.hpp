#pragma once

#include <orbit/core/Types.hpp>

namespace orbit::terrain
{
struct TerrainClimate
{
    f32 temperatureC{15.0F};
    f32 humidity{0.5F};
    f32 precipitation{0.5F};
    f32 continentality{0.5F};
};

struct BiomeWeights
{
    f32 ocean{0.0F};
    f32 desert{0.0F};
    f32 grassland{1.0F};
    f32 temperateForest{0.0F};
    f32 borealForest{0.0F};
    f32 tundra{0.0F};
    f32 alpine{0.0F};
    f32 wetland{0.0F};
};

struct TerrainSample
{
    f64 elevationMeters{0.0};
    f64 coarseElevationMeters{0.0};
    TerrainClimate climate{};
    BiomeWeights biomes{};
};

[[nodiscard]] BiomeWeights NormalizeBiomeWeights(
    const BiomeWeights& weights) noexcept;

[[nodiscard]] BiomeWeights LerpBiomeWeights(
    const BiomeWeights& a,
    const BiomeWeights& b,
    f64 t) noexcept;

[[nodiscard]] TerrainClimate LerpTerrainClimate(
    const TerrainClimate& a,
    const TerrainClimate& b,
    f64 t) noexcept;

[[nodiscard]] TerrainSample LerpTerrainSample(
    const TerrainSample& a,
    const TerrainSample& b,
    f64 t) noexcept;

[[nodiscard]] BiomeWeights ClassifyBiomeWeights(
    const TerrainClimate& climate,
    f64 elevationMeters,
    f64 seaLevelMeters = 0.0) noexcept;
} // namespace orbit::terrain
