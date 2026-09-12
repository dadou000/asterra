#include <orbit/world/PlanetTileNeighborhood.hpp>

#include <algorithm>
#include <limits>

namespace orbit::world
{
namespace
{
[[nodiscard]] math::Double3 CubeToUnitDirectionUnclamped(
    const CubeFace face,
    const math::Double2 uv) noexcept
{
    math::Double3 cube{};

    switch (face)
    {
    case CubeFace::PositiveX:
        cube = {1.0, uv.y, -uv.x};
        break;
    case CubeFace::NegativeX:
        cube = {-1.0, uv.y, uv.x};
        break;
    case CubeFace::PositiveY:
        cube = {uv.x, 1.0, -uv.y};
        break;
    case CubeFace::NegativeY:
        cube = {uv.x, -1.0, uv.y};
        break;
    case CubeFace::PositiveZ:
        cube = {uv.x, uv.y, 1.0};
        break;
    case CubeFace::NegativeZ:
        cube = {-uv.x, uv.y, -1.0};
        break;
    }

    return math::Normalize(cube);
}
} // namespace

PlanetTileId OffsetTile(
    const PlanetTileId tile,
    const i32 dx,
    const i32 dy) noexcept
{
    const CubeCoordinate center =
        TileCenter(tile);

    const CubeBounds bounds =
        TileBounds(tile);

    const f64 tileWidthU =
        bounds.maximumUv.x -
        bounds.minimumUv.x;

    const f64 tileWidthV =
        bounds.maximumUv.y -
        bounds.minimumUv.y;

    const math::Double2 offsetUv{
        center.uv.x +
            static_cast<f64>(dx) * tileWidthU,
        center.uv.y +
            static_cast<f64>(dy) * tileWidthV
    };

    const math::Double3 direction =
        CubeToUnitDirectionUnclamped(
            center.face,
            offsetUv);

    return TileForDirection(
        direction,
        tile.level);
}

std::vector<PlanetTileId> TileNeighborhood(
    const PlanetTileId center,
    const u32 radius)
{
    std::vector<PlanetTileId> result;

    const u32 maximumOffset =
        static_cast<u32>(
            (std::numeric_limits<i32>::max)());

    const i32 boundedRadius =
        static_cast<i32>(
            (std::min)(
                radius,
                maximumOffset));

    const std::size_t side =
        static_cast<std::size_t>(
            boundedRadius) *
            2U +
        1U;

    if (side <=
        (std::numeric_limits<std::size_t>::max)() /
            side)
    {
        result.reserve(side * side);
    }

    for (i64 y = -static_cast<i64>(boundedRadius);
         y <= static_cast<i64>(boundedRadius);
         ++y)
    {
        for (i64 x = -static_cast<i64>(boundedRadius);
             x <= static_cast<i64>(boundedRadius);
             ++x)
        {
            const PlanetTileId candidate =
                OffsetTile(
                    center,
                    static_cast<i32>(x),
                    static_cast<i32>(y));

            if (std::find(
                    result.begin(),
                    result.end(),
                    candidate) ==
                result.end())
            {
                result.push_back(candidate);
            }
        }
    }

    return result;
}
} // namespace orbit::world
