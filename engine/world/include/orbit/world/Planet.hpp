#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

namespace orbit::world
{
struct PlanetIdTag;
using PlanetId = core::StrongId<PlanetIdTag>;

enum class CubeFace : u8
{
    PositiveX,
    NegativeX,
    PositiveY,
    NegativeY,
    PositiveZ,
    NegativeZ
};

struct CubeCoordinate
{
    CubeFace face{CubeFace::PositiveX};
    math::Double2 uv{};
};

struct CubeBounds
{
    CubeFace face{CubeFace::PositiveX};
    math::Double2 minimumUv{};
    math::Double2 maximumUv{};
};

struct PlanetTileId
{
    CubeFace face{CubeFace::PositiveX};
    u8 level{0};
    u32 x{0};
    u32 y{0};

    [[nodiscard]] constexpr bool operator==(
        const PlanetTileId&) const noexcept = default;
};

struct PlanetDefinition
{
    f64 radiusMeters{6'000'000.0};

    // Stable identity is authored/persisted. It must never be derived from
    // camera, render, cache-slot or process-local state.
    PlanetId id{};

    // Root for deterministic procedural domains on this planet. Individual
    // systems derive domain/page seeds from this value rather than owning
    // independent random generators.
    u64 generationSeed{0};
};

struct SurfaceFrame
{
    math::Double3 east{};
    math::Double3 north{};
    math::Double3 up{};
};

[[nodiscard]] math::Double3 CubeToUnitDirection(
    const CubeCoordinate& coordinate) noexcept;

[[nodiscard]] CubeCoordinate UnitDirectionToCube(
    const math::Double3& direction) noexcept;

[[nodiscard]] PlanetTileId TileForDirection(
    const math::Double3& direction,
    u8 level) noexcept;

[[nodiscard]] CubeCoordinate TileCenter(
    const PlanetTileId& tile) noexcept;

[[nodiscard]] CubeBounds TileBounds(
    const PlanetTileId& tile) noexcept;

[[nodiscard]] f64 ApproximateTileWidthMeters(
    const PlanetDefinition& planet,
    const PlanetTileId& tile) noexcept;

[[nodiscard]] f64 HorizonArcDistanceMeters(
    f64 sphereRadiusMeters,
    f64 observerRadiusMeters) noexcept;

[[nodiscard]] SurfaceFrame MakeSurfaceFrame(
    const math::Double3& upDirection) noexcept;

[[nodiscard]] SurfaceFrame TransportSurfaceFrameToDirection(
    const SurfaceFrame& frame,
    const math::Double3& targetUpDirection) noexcept;

[[nodiscard]] math::Double3 DirectionAtSurfaceOffset(
    const PlanetDefinition& planet,
    const SurfaceFrame& frame,
    const math::Double2& offsetMeters) noexcept;

[[nodiscard]] SurfaceFrame SurfaceFrameAtOffset(
    const PlanetDefinition& planet,
    const SurfaceFrame& frame,
    const math::Double2& offsetMeters) noexcept;

[[nodiscard]] math::Double2 SurfaceOffsetBetweenDirections(
    const PlanetDefinition& planet,
    const SurfaceFrame& fromFrame,
    const math::Double3& toDirection) noexcept;
} // namespace orbit::world
