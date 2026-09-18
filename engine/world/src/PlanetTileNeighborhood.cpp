#include <orbit/world/PlanetTileNeighborhood.hpp>

#include <orbit/world/CubeProjection.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

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
[[nodiscard]] math::Double2 EdgeUv(
    const CubeBounds& bounds,
    const TileEdge edge,
    const f64 t) noexcept
{
    const f64 clampedT =
        std::clamp(t, 0.0, 1.0);

    const f64 u =
        bounds.minimumUv.x +
        (bounds.maximumUv.x -
         bounds.minimumUv.x) *
            clampedT;

    const f64 v =
        bounds.minimumUv.y +
        (bounds.maximumUv.y -
         bounds.minimumUv.y) *
            clampedT;

    switch (edge)
    {
    case TileEdge::North:
        return {u, bounds.minimumUv.y};
    case TileEdge::East:
        return {bounds.maximumUv.x, v};
    case TileEdge::South:
        return {u, bounds.maximumUv.y};
    case TileEdge::West:
        return {bounds.minimumUv.x, v};
    }

    return {};
}

[[nodiscard]] TileEdge ClosestEdge(
    const CubeBounds& bounds,
    const math::Double2 uv) noexcept
{
    const std::array<f64, 4> distances{
        std::abs(uv.y - bounds.minimumUv.y),
        std::abs(uv.x - bounds.maximumUv.x),
        std::abs(uv.y - bounds.maximumUv.y),
        std::abs(uv.x - bounds.minimumUv.x)
    };

    const auto best =
        std::min_element(
            distances.begin(),
            distances.end());

    return static_cast<TileEdge>(
        static_cast<u8>(
            std::distance(
                distances.begin(),
                best)));
}

[[nodiscard]] f64 EdgeParameter(
    const CubeBounds& bounds,
    const TileEdge edge,
    const math::Double2 uv) noexcept
{
    switch (edge)
    {
    case TileEdge::North:
    case TileEdge::South:
    {
        const f64 width =
            bounds.maximumUv.x -
            bounds.minimumUv.x;

        return width > 0.0
            ? (uv.x - bounds.minimumUv.x) / width
            : 0.0;
    }

    case TileEdge::East:
    case TileEdge::West:
    {
        const f64 height =
            bounds.maximumUv.y -
            bounds.minimumUv.y;

        return height > 0.0
            ? (uv.y - bounds.minimumUv.y) / height
            : 0.0;
    }
    }

    return 0.0;
}

[[nodiscard]] std::pair<i32, i32> EdgeOffset(
    const TileEdge edge) noexcept
{
    switch (edge)
    {
    case TileEdge::North:
        return {0, -1};
    case TileEdge::East:
        return {1, 0};
    case TileEdge::South:
        return {0, 1};
    case TileEdge::West:
        return {-1, 0};
    }

    return {};
}

[[nodiscard]] TileGridOffset InwardNormal(
    const TileEdge edge) noexcept
{
    switch (edge)
    {
    case TileEdge::North:
        return {0, 1};
    case TileEdge::East:
        return {-1, 0};
    case TileEdge::South:
        return {0, -1};
    case TileEdge::West:
        return {1, 0};
    }

    return {};
}

[[nodiscard]] TileGridOffset PositiveTangent(
    const TileEdge edge) noexcept
{
    switch (edge)
    {
    case TileEdge::North:
    case TileEdge::South:
        return {1, 0};
    case TileEdge::East:
    case TileEdge::West:
        return {0, 1};
    }

    return {};
}

[[nodiscard]] bool ExitsThroughEdge(
    const TileEdge edge,
    const TileGridOffset flow) noexcept
{
    switch (edge)
    {
    case TileEdge::North:
        return flow.dy < 0;
    case TileEdge::East:
        return flow.dx > 0;
    case TileEdge::South:
        return flow.dy > 0;
    case TileEdge::West:
        return flow.dx < 0;
    }

    return false;
}

[[nodiscard]] i8 TangentComponent(
    const TileEdge edge,
    const TileGridOffset flow) noexcept
{
    switch (edge)
    {
    case TileEdge::North:
    case TileEdge::South:
        return flow.dx;
    case TileEdge::East:
    case TileEdge::West:
        return flow.dy;
    }

    return 0;
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
TileEdgeNeighborMapping NeighborAcrossTileEdge(
    const PlanetTileId tile,
    const TileEdge edge) noexcept
{
    const auto [dx, dy] =
        EdgeOffset(edge);

    const PlanetTileId neighbor =
        OffsetTile(
            tile,
            dx,
            dy);

    const CubeBounds sourceBounds =
        TileBounds(tile);

    const CubeBounds neighborBounds =
        TileBounds(neighbor);

    const math::Double3 midpointDirection =
        CubeToUnitDirection({
            .face = tile.face,
            .uv = EdgeUv(
                sourceBounds,
                edge,
                0.5)
        });

    const auto midpointOnNeighbor =
        ProjectDirectionToCubeFace(
            midpointDirection,
            neighbor.face);

    if (!midpointOnNeighbor)
    {
        return {
            .tile = neighbor,
            .edge = TileEdge::North,
            .reverseSamples = false
        };
    }

    const TileEdge neighborEdge =
        ClosestEdge(
            neighborBounds,
            midpointOnNeighbor->uv);

    const auto parameterOnNeighbor =
        [&](const f64 sourceT)
        {
            const math::Double3 direction =
                CubeToUnitDirection({
                    .face = tile.face,
                    .uv = EdgeUv(
                        sourceBounds,
                        edge,
                        sourceT)
                });

            const auto projected =
                ProjectDirectionToCubeFace(
                    direction,
                    neighbor.face);

            if (!projected)
            {
                return 0.0;
            }

            return EdgeParameter(
                neighborBounds,
                neighborEdge,
                projected->uv);
        };

    const f64 first =
        parameterOnNeighbor(0.25);

    const f64 second =
        parameterOnNeighbor(0.75);

    return {
        .tile = neighbor,
        .edge = neighborEdge,
        .reverseSamples =
            second < first
    };
}

u32 RemapTileEdgeSampleIndex(
    const TileEdgeNeighborMapping& mapping,
    const u32 sourceIndex,
    const u32 resolution) noexcept
{
    if (resolution == 0U)
    {
        return 0U;
    }

    const u32 bounded =
        std::min(
            sourceIndex,
            resolution - 1U);

    return mapping.reverseSamples
        ? resolution - 1U - bounded
        : bounded;
}

TileGridOffset TransformFlowAcrossTileEdge(
    const TileEdge sourceEdge,
    const TileEdgeNeighborMapping& mapping,
    const TileGridOffset sourceFlow) noexcept
{
    if (!ExitsThroughEdge(
            sourceEdge,
            sourceFlow))
    {
        return {};
    }

    i8 tangent =
        TangentComponent(
            sourceEdge,
            sourceFlow);

    if (mapping.reverseSamples)
    {
        tangent =
            static_cast<i8>(
                -tangent);
    }

    const TileGridOffset inward =
        InwardNormal(
            mapping.edge);

    const TileGridOffset positiveTangent =
        PositiveTangent(
            mapping.edge);

    return {
        .dx = static_cast<i8>(
            inward.dx +
            positiveTangent.dx *
                tangent),
        .dy = static_cast<i8>(
            inward.dy +
            positiveTangent.dy *
                tangent)
    };
}
} // namespace orbit::world
