#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/world/Planet.hpp>

#include <vector>

namespace orbit::world
{
// Local page-grid convention used by physical page exchange. North is the
// minimum-v / y-1 edge, east is maximum-u / x+1, south is maximum-v / y+1,
// and west is minimum-u / x-1. These names describe page-local orientation;
// they are not global geographic north/east at arbitrary cube faces.
enum class TileEdge : u8
{
    North,
    East,
    South,
    West
};

struct TileGridOffset
{
    i8 dx{0};
    i8 dy{0};

    [[nodiscard]] constexpr bool operator==(
        const TileGridOffset&) const noexcept = default;
};

struct TileEdgeNeighborMapping
{
    PlanetTileId tile{};
    TileEdge edge{TileEdge::North};
    bool reverseSamples{false};
};

[[nodiscard]] PlanetTileId OffsetTile(
    PlanetTileId tile,
    i32 dx,
    i32 dy) noexcept;

[[nodiscard]] std::vector<PlanetTileId> TileNeighborhood(
    PlanetTileId center,
    u32 radius);

// Returns the physical neighbor across one page edge plus the receiving edge
// and edge-sample orientation. This is derived from the cube projection, not a
// hard-coded face table, so every face rotation shares the same contract.
[[nodiscard]] TileEdgeNeighborMapping NeighborAcrossTileEdge(
    PlanetTileId tile,
    TileEdge edge) noexcept;

[[nodiscard]] u32 RemapTileEdgeSampleIndex(
    const TileEdgeNeighborMapping& mapping,
    u32 sourceIndex,
    u32 resolution) noexcept;

// Transform a D8 flow step that exits sourceEdge into the receiving page's
// local grid orientation. A non-exiting sourceFlow returns {0,0}.
[[nodiscard]] TileGridOffset TransformFlowAcrossTileEdge(
    TileEdge sourceEdge,
    const TileEdgeNeighborMapping& mapping,
    TileGridOffset sourceFlow) noexcept;
} // namespace orbit::world
