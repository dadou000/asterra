#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/world/Planet.hpp>

#include <cmath>
#include <limits>

namespace orbit::terrain
{
// Canonical physical surface location for terrain processes.
//
// The location is expressed in planet/body space, never in a cube face,
// clipmap ring, viewport or GPU cache slot. Cube/clipmap coordinates are
// storage/view projections derived from this value.
struct PlanetSurfacePosition
{
    world::PlanetId planet{};
    math::Double3 unitDirection{0.0, 1.0, 0.0};
    f64 radialOffsetMeters{0.0};

    [[nodiscard]] bool IsValid() const noexcept
    {
        return
            planet.IsValid() &&
            std::isfinite(unitDirection.x) &&
            std::isfinite(unitDirection.y) &&
            std::isfinite(unitDirection.z) &&
            math::LengthSquared(unitDirection) > 0.0 &&
            std::isfinite(radialOffsetMeters);
    }

    [[nodiscard]] bool operator==(
        const PlanetSurfacePosition&) const noexcept = default;
};

// LOD/sample support expressed in physical meters. Terrain processes may use
// this to cut procedural frequencies that cannot be resolved by the request.
// It is deliberately independent of a render/clipmap level number.
struct TerrainSampleFootprint
{
    f64 diameterMeters{1.0};

    [[nodiscard]] bool IsValid() const noexcept
    {
        return
            std::isfinite(diameterMeters) &&
            diameterMeters > 0.0;
    }

    [[nodiscard]] constexpr f64
    MinimumResolvedWavelengthMeters() const noexcept
    {
        return diameterMeters * 2.0;
    }
};

[[nodiscard]] inline PlanetSurfacePosition
CanonicalizeSurfacePosition(
    PlanetSurfacePosition position) noexcept
{
    if (!std::isfinite(position.unitDirection.x) ||
        !std::isfinite(position.unitDirection.y) ||
        !std::isfinite(position.unitDirection.z) ||
        math::LengthSquared(position.unitDirection) <= 0.0)
    {
        position.unitDirection = {0.0, 1.0, 0.0};
    }
    else
    {
        position.unitDirection =
            math::Normalize(position.unitDirection);
    }

    if (!std::isfinite(position.radialOffsetMeters))
    {
        position.radialOffsetMeters = 0.0;
    }

    return position;
}

[[nodiscard]] inline world::SurfaceFrame
SurfaceTangentFrame(
    const PlanetSurfacePosition& position) noexcept
{
    return world::MakeSurfaceFrame(
        CanonicalizeSurfacePosition(position).
            unitDirection);
}

[[nodiscard]] inline world::SurfaceFrame
AlignSurfaceTangentFrame(
    const PlanetSurfacePosition& position,
    const world::SurfaceFrame& frame) noexcept
{
    return world::TransportSurfaceFrameToDirection(
        frame,
        CanonicalizeSurfacePosition(position).
            unitDirection);
}

[[nodiscard]] inline math::Double3
BodyLocalPoint(
    const world::PlanetDefinition& planet,
    const PlanetSurfacePosition& position) noexcept
{
    const PlanetSurfacePosition canonical =
        CanonicalizeSurfacePosition(position);

    return canonical.unitDirection *
        (planet.radiusMeters +
         canonical.radialOffsetMeters);
}

[[nodiscard]] inline PlanetSurfacePosition
SurfacePositionFromCube(
    const world::PlanetId planet,
    const world::CubeCoordinate& coordinate,
    const f64 radialOffsetMeters = 0.0) noexcept
{
    return CanonicalizeSurfacePosition({
        .planet = planet,
        .unitDirection =
            world::CubeToUnitDirection(coordinate),
        .radialOffsetMeters = radialOffsetMeters
    });
}

[[nodiscard]] inline world::PlanetTileId
PhysicalTileForPosition(
    const PlanetSurfacePosition& position,
    const u8 level) noexcept
{
    return world::TileForDirection(
        CanonicalizeSurfacePosition(position).
            unitDirection,
        level);
}

// Move in the origin frame's tangent plane using geodesic distance on the
// reference sphere. The returned physical position remains independent of any
// cube-face/clipmap representation. Supplying the frame explicitly preserves
// authored/regional tangent orientation while keeping the position canonical.
[[nodiscard]] inline PlanetSurfacePosition
OffsetSurfacePosition(
    const world::PlanetDefinition& planet,
    const PlanetSurfacePosition& origin,
    const world::SurfaceFrame& originFrame,
    const math::Double2& offsetMeters) noexcept
{
    PlanetSurfacePosition result =
        CanonicalizeSurfacePosition(origin);

    const world::SurfaceFrame alignedFrame =
        AlignSurfaceTangentFrame(
            result,
            originFrame);

    result.unitDirection =
        world::DirectionAtSurfaceOffset(
            planet,
            alignedFrame,
            offsetMeters);

    return CanonicalizeSurfacePosition(result);
}

[[nodiscard]] inline PlanetSurfacePosition
OffsetSurfacePosition(
    const world::PlanetDefinition& planet,
    const PlanetSurfacePosition& origin,
    const math::Double2& offsetMeters) noexcept
{
    const PlanetSurfacePosition canonical =
        CanonicalizeSurfacePosition(origin);

    return OffsetSurfacePosition(
        planet,
        canonical,
        world::MakeSurfaceFrame(
            canonical.unitDirection),
        offsetMeters);
}

// Inverse of OffsetSurfacePosition in the same tangent orientation. A planet
// mismatch has no meaningful surface displacement and returns infinities so
// callers cannot accidentally treat another body's coordinates as local.
[[nodiscard]] inline math::Double2
SurfaceOffsetBetweenPositions(
    const world::PlanetDefinition& planet,
    const PlanetSurfacePosition& origin,
    const world::SurfaceFrame& originFrame,
    const PlanetSurfacePosition& target) noexcept
{
    const PlanetSurfacePosition canonicalOrigin =
        CanonicalizeSurfacePosition(origin);
    const PlanetSurfacePosition canonicalTarget =
        CanonicalizeSurfacePosition(target);

    if (canonicalOrigin.planet.IsValid() &&
        canonicalTarget.planet.IsValid() &&
        canonicalOrigin.planet != canonicalTarget.planet)
    {
        const f64 infinity =
            std::numeric_limits<f64>::infinity();
        return {infinity, infinity};
    }

    const world::SurfaceFrame alignedFrame =
        AlignSurfaceTangentFrame(
            canonicalOrigin,
            originFrame);

    return world::SurfaceOffsetBetweenDirections(
        planet,
        alignedFrame,
        canonicalTarget.unitDirection);
}

[[nodiscard]] inline math::Double2
SurfaceOffsetBetweenPositions(
    const world::PlanetDefinition& planet,
    const PlanetSurfacePosition& origin,
    const PlanetSurfacePosition& target) noexcept
{
    const PlanetSurfacePosition canonicalOrigin =
        CanonicalizeSurfacePosition(origin);

    return SurfaceOffsetBetweenPositions(
        planet,
        canonicalOrigin,
        world::MakeSurfaceFrame(
            canonicalOrigin.unitDirection),
        target);
}
} // namespace orbit::terrain
