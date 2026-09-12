#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::world
{
namespace
{
[[nodiscard]] f64 ClampUnit(const f64 value) noexcept
{
    return std::clamp(value, -1.0, 1.0);
}

[[nodiscard]] u32 TileCount(const u8 level) noexcept
{
    const u8 safeLevel = std::min<u8>(level, 30);
    return 1U << safeLevel;
}

[[nodiscard]] u32 CoordinateToTileIndex(
    const f64 coordinate,
    const u32 tileCount) noexcept
{
    const f64 normalized =
        std::clamp(coordinate * 0.5 + 0.5, 0.0, 1.0);

    if (normalized >= 1.0)
    {
        return tileCount - 1;
    }

    return static_cast<u32>(
        normalized * static_cast<f64>(tileCount));
}
} // namespace

math::Double3 CubeToUnitDirection(
    const CubeCoordinate& coordinate) noexcept
{
    const f64 u = ClampUnit(coordinate.uv.x);
    const f64 v = ClampUnit(coordinate.uv.y);

    math::Double3 cube{};

    switch (coordinate.face)
    {
    case CubeFace::PositiveX:
        cube = {1.0, v, -u};
        break;
    case CubeFace::NegativeX:
        cube = {-1.0, v, u};
        break;
    case CubeFace::PositiveY:
        cube = {u, 1.0, -v};
        break;
    case CubeFace::NegativeY:
        cube = {u, -1.0, v};
        break;
    case CubeFace::PositiveZ:
        cube = {u, v, 1.0};
        break;
    case CubeFace::NegativeZ:
        cube = {-u, v, -1.0};
        break;
    }

    return math::Normalize(cube);
}

CubeCoordinate UnitDirectionToCube(
    const math::Double3& direction) noexcept
{
    const math::Double3 unit = math::Normalize(direction);

    const f64 ax = std::abs(unit.x);
    const f64 ay = std::abs(unit.y);
    const f64 az = std::abs(unit.z);

    if (ax <= 0.0 && ay <= 0.0 && az <= 0.0)
    {
        return {};
    }

    CubeCoordinate result{};

    if (ax >= ay && ax >= az)
    {
        if (unit.x >= 0.0)
        {
            result.face = CubeFace::PositiveX;
            result.uv = {
                -unit.z / ax,
                unit.y / ax
            };
        }
        else
        {
            result.face = CubeFace::NegativeX;
            result.uv = {
                unit.z / ax,
                unit.y / ax
            };
        }
    }
    else if (ay >= ax && ay >= az)
    {
        if (unit.y >= 0.0)
        {
            result.face = CubeFace::PositiveY;
            result.uv = {
                unit.x / ay,
                -unit.z / ay
            };
        }
        else
        {
            result.face = CubeFace::NegativeY;
            result.uv = {
                unit.x / ay,
                unit.z / ay
            };
        }
    }
    else
    {
        if (unit.z >= 0.0)
        {
            result.face = CubeFace::PositiveZ;
            result.uv = {
                unit.x / az,
                unit.y / az
            };
        }
        else
        {
            result.face = CubeFace::NegativeZ;
            result.uv = {
                -unit.x / az,
                unit.y / az
            };
        }
    }

    result.uv.x = ClampUnit(result.uv.x);
    result.uv.y = ClampUnit(result.uv.y);
    return result;
}

PlanetTileId TileForDirection(
    const math::Double3& direction,
    const u8 level) noexcept
{
    const u8 safeLevel = std::min<u8>(level, 30);
    const CubeCoordinate cube = UnitDirectionToCube(direction);
    const u32 count = TileCount(safeLevel);

    return {
        .face = cube.face,
        .level = safeLevel,
        .x = CoordinateToTileIndex(cube.uv.x, count),
        .y = CoordinateToTileIndex(cube.uv.y, count)
    };
}

CubeCoordinate TileCenter(const PlanetTileId& tile) noexcept
{
    const u8 safeLevel = std::min<u8>(tile.level, 30);
    const u32 count = TileCount(safeLevel);

    const u32 x = std::min(tile.x, count - 1);
    const u32 y = std::min(tile.y, count - 1);

    const f64 inverseCount = 1.0 / static_cast<f64>(count);

    return {
        .face = tile.face,
        .uv = {
            (static_cast<f64>(x) + 0.5) * inverseCount * 2.0 - 1.0,
            (static_cast<f64>(y) + 0.5) * inverseCount * 2.0 - 1.0
        }
    };
}

f64 ApproximateTileWidthMeters(
    const PlanetDefinition& planet,
    const PlanetTileId& tile) noexcept
{
    const u8 safeLevel = std::min<u8>(tile.level, 30);
    const u32 count = TileCount(safeLevel);
    const u32 x = std::min(tile.x, count - 1);
    const u32 y = std::min(tile.y, count - 1);

    const f64 inverseCount = 1.0 / static_cast<f64>(count);
    const f64 v =
        (static_cast<f64>(y) + 0.5) * inverseCount * 2.0 - 1.0;

    const f64 u0 =
        static_cast<f64>(x) * inverseCount * 2.0 - 1.0;
    const f64 u1 =
        static_cast<f64>(x + 1U) * inverseCount * 2.0 - 1.0;

    const math::Double3 left = CubeToUnitDirection({
        .face = tile.face,
        .uv = {u0, v}
    });

    const math::Double3 right = CubeToUnitDirection({
        .face = tile.face,
        .uv = {u1, v}
    });

    const f64 cosine = std::clamp(
        math::Dot(left, right),
        -1.0,
        1.0);

    return std::acos(cosine) * planet.radiusMeters;
}

SurfaceFrame MakeSurfaceFrame(
    const math::Double3& upDirection) noexcept
{
    math::Double3 up = math::Normalize(upDirection);

    if (math::LengthSquared(up) <= 0.0)
    {
        up = {0.0, 0.0, 1.0};
    }

    const math::Double3 reference =
        std::abs(up.y) < 0.95
            ? math::Double3{0.0, 1.0, 0.0}
            : math::Double3{1.0, 0.0, 0.0};

    const math::Double3 east =
        math::Normalize(math::Cross(reference, up));

    const math::Double3 north =
        math::Normalize(math::Cross(up, east));

    return {
        .east = east,
        .north = north,
        .up = up
    };
}

math::Double3 DirectionAtSurfaceOffset(
    const PlanetDefinition& planet,
    const SurfaceFrame& frame,
    const math::Double2& offsetMeters) noexcept
{
    const math::Double3 tangent =
        frame.east * offsetMeters.x +
        frame.north * offsetMeters.y;

    const f64 distance = math::Length(tangent);

    if (distance <= 0.0 ||
        planet.radiusMeters <= 0.0)
    {
        return math::Normalize(frame.up);
    }

    const f64 angle = distance / planet.radiusMeters;
    const math::Double3 tangentDirection = tangent / distance;

    return math::Normalize(
        frame.up * std::cos(angle) +
        tangentDirection * std::sin(angle));
}
} // namespace orbit::world
