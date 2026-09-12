#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/world/Planet.hpp>

#include <cstddef>
#include <vector>

namespace orbit::terrain_cache
{
struct TerrainPageDesc
{
    world::PlanetTileId tile{};
    u32 resolution{65};

    [[nodiscard]] constexpr bool operator==(
        const TerrainPageDesc&) const noexcept = default;
};

struct TerrainPageDescHash
{
    [[nodiscard]] std::size_t operator()(
        const TerrainPageDesc& desc) const noexcept;
};

struct TerrainPage
{
    TerrainPageDesc desc{};
    f64 approximateSampleSpacingMeters{0.0};
    std::vector<f32> elevationMeters;

    [[nodiscard]] f32 At(u32 x, u32 y) const;

    [[nodiscard]] f32 SampleDirection(
        const math::Double3& unitDirection) const noexcept;
};

} // namespace orbit::terrain_cache
