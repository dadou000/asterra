#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/universe/BodyRegistry.hpp>
#include <orbit/world/Planet.hpp>

#include <memory>
#include <optional>
#include <unordered_map>

namespace orbit::terrain
{
class TerrainSource;
}

namespace orbit::surface
{
struct SurfaceCoordinate
{
    // Direction from the body center in the body frame.
    math::Double3 unitDirection{
        0.0,
        1.0,
        0.0
    };

    // Radial offset from the reference shape. This is intentionally
    // distinct from terrain elevation: it is a geometric coordinate
    // usable by bodies that have no terrain capability.
    f64 radialOffsetMeters{0.0};
};

struct TerrainSurfaceCapability
{
    universe::BodyId body{};
    std::shared_ptr<const terrain::TerrainSource>
        terrain;
};

// CPU-authoritative capability attachment point between generic
// celestial bodies and terrain-specific surface implementations.
class SurfaceRegistry
{
public:
    explicit SurfaceRegistry(
        const universe::BodyRegistry& bodies);

    [[nodiscard]] std::optional<world::PlanetDefinition>
    SphericalPlanetDefinition(
        universe::BodyId body) const noexcept;

    [[nodiscard]] std::optional<math::Double3>
    BodyPointFromSurface(
        universe::BodyId body,
        const SurfaceCoordinate& coordinate) const noexcept;

    [[nodiscard]] std::optional<SurfaceCoordinate>
    SurfaceFromBodyPoint(
        universe::BodyId body,
        const math::Double3& bodyPoint) const noexcept;

    void AttachTerrain(
        universe::BodyId body,
        std::shared_ptr<const terrain::TerrainSource>
            terrain);

    void DetachTerrain(
        universe::BodyId body) noexcept;

    [[nodiscard]] const TerrainSurfaceCapability*
    FindTerrainSurface(
        universe::BodyId body) const noexcept;

private:
    [[nodiscard]] std::optional<f64>
    ReferenceRadiusAlongDirection(
        universe::BodyId body,
        const math::Double3& unitDirection) const noexcept;

    const universe::BodyRegistry& bodies_;
    std::unordered_map<
        universe::BodyId,
        TerrainSurfaceCapability>
        terrainSurfaces_;
};
} // namespace orbit::surface
