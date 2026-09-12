#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/world/Planet.hpp>

#include <vector>

namespace orbit::world
{
[[nodiscard]] PlanetTileId OffsetTile(
    PlanetTileId tile,
    i32 dx,
    i32 dy) noexcept;

[[nodiscard]] std::vector<PlanetTileId> TileNeighborhood(
    PlanetTileId center,
    u32 radius);
} // namespace orbit::world
