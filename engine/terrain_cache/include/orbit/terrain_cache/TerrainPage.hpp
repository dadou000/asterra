#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain/TerrainFields.hpp>
#include <orbit/world/Planet.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace orbit::terrain_cache
{
struct TerrainPageDesc
{
    world::PlanetId planet{};
    world::PlanetTileId tile{};
    u32 resolution{65};
    terrain::TerrainGenerationRevisions revisions{};

    [[nodiscard]] constexpr terrain::PhysicalTerrainPageKey
    PhysicalKey() const noexcept
    {
        return {
            .address = {
                .planet = planet,
                .tile = tile
            },
            .resolution = resolution,
            .revisions = revisions
        };
    }

    [[nodiscard]] constexpr bool operator==(
        const TerrainPageDesc&) const noexcept = default;
};

struct TerrainPageDescHash
{
    [[nodiscard]] std::size_t operator()(
        const TerrainPageDesc& desc) const noexcept;
};

// Storage-only form: float heights/depth, float climate and RGBA8 biomes.
// TerrainPageCache budgets the actual sizeof(CachedTerrainSample).
struct CachedTerrainSample
{
    f32 elevationMeters{0.0F};
    f32 coarseElevationMeters{0.0F};
    terrain::TerrainClimate climate{};
    std::array<u8, 8> quantizedBiomeWeights{};
    f32 standingWaterDepthMeters{0.0F};
};

[[nodiscard]] CachedTerrainSample ToCachedSample(
    const terrain::TerrainSample& sample) noexcept;

[[nodiscard]] terrain::TerrainSample FromCachedSample(
    const CachedTerrainSample& sample) noexcept;

struct TerrainPage
{
    TerrainPageDesc desc{};
    f64 approximateSampleSpacingMeters{0.0};
    std::vector<CachedTerrainSample> samples;

    [[nodiscard]] terrain::TerrainSample SampleAt(
        u32 x,
        u32 y) const;

    [[nodiscard]] f32 At(
        u32 x,
        u32 y) const;

    [[nodiscard]] terrain::TerrainSample SampleDirection(
        const math::Double3& unitDirection) const noexcept;
};
} // namespace orbit::terrain_cache
