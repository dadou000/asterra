#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
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
    world::PlanetTileId tile{};
    u32 resolution{65};
    u64 sourceRevision{0};

    [[nodiscard]] constexpr bool operator==(
        const TerrainPageDesc&) const noexcept = default;
};

struct TerrainPageDescHash
{
    [[nodiscard]] std::size_t operator()(
        const TerrainPageDesc& desc) const noexcept;
};

// Storage-only, lossy-compressed form of terrain::TerrainSample: 32
// bytes instead of 64. Elevations only need single-precision at page
// scale (a handful of km across at most, so f32's ~7 significant
// digits keep sub-millimeter error), and biome weights are a
// normalized blend so 8-bit quantization (1/255 steps) is well below
// visible/gameplay-relevant precision. This halves the resident
// bytes per cached page, which is what the page cache's byte budget
// is actually rationing -- see TerrainPageCache.
struct CachedTerrainSample
{
    f32 elevationMeters{0.0F};
    f32 coarseElevationMeters{0.0F};
    terrain::TerrainClimate climate{};
    std::array<u8, 8> quantizedBiomeWeights{};
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
