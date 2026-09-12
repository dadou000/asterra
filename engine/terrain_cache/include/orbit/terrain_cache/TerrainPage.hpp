#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/world/Planet.hpp>

#include <vector>

namespace orbit::terrain_cache
{
struct TerrainPageDesc
{
    world::PlanetTileId tile{};
    u32 resolution{65};
};

struct TerrainPage
{
    TerrainPageDesc desc{};
    f64 approximateSampleSpacingMeters{0.0};
    std::vector<f32> elevationMeters;

    [[nodiscard]] f32 At(u32 x, u32 y) const;
};

} // namespace orbit::terrain_cache
