#include <orbit/surface/SurfaceRegistry.hpp>

#include <orbit/terrain/TerrainSource.hpp>

#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace orbit::surface
{
SurfaceRegistry::SurfaceRegistry(
    const universe::BodyRegistry& bodies)
    : bodies_(bodies)
{
}

std::optional<world::PlanetDefinition>
SurfaceRegistry::SphericalPlanetDefinition(
    const universe::BodyId body) const noexcept
{
    const universe::CelestialBody* record =
        bodies_.FindBody(body);

    if (record == nullptr)
    {
        return std::nullopt;
    }

    const auto* sphere =
        std::get_if<
            universe::SphereShape>(
                &record->shape);

    if (sphere == nullptr)
    {
        return std::nullopt;
    }

    return world::PlanetDefinition{
        .radiusMeters =
            sphere->radiusMeters
    };
}

std::optional<f64>
SurfaceRegistry::ReferenceRadiusAlongDirection(
    const universe::BodyId body,
    const math::Double3& unitDirection) const noexcept
{
    const universe::CelestialBody* record =
        bodies_.FindBody(body);

    if (record == nullptr)
    {
        return std::nullopt;
    }

    const math::Double3 direction =
        math::Normalize(unitDirection);

    if (math::LengthSquared(direction) <= 0.0)
    {
        return std::nullopt;
    }

    return std::visit(
        [&direction](const auto& shape) -> f64
        {
            using Shape =
                std::decay_t<
                    decltype(shape)>;

            if constexpr (
                std::is_same_v<
                    Shape,
                    universe::SphereShape>)
            {
                return shape.radiusMeters;
            }
            else
            {
                const f64 x =
                    direction.x /
                    shape.radiiMeters.x;
                const f64 y =
                    direction.y /
                    shape.radiiMeters.y;
                const f64 z =
                    direction.z /
                    shape.radiiMeters.z;

                const f64 denominator =
                    std::sqrt(
                        x * x +
                        y * y +
                        z * z);

                if (denominator <= 0.0)
                {
                    return 0.0;
                }

                return 1.0 / denominator;
            }
        },
        record->shape);
}

std::optional<math::Double3>
SurfaceRegistry::BodyPointFromSurface(
    const universe::BodyId body,
    const SurfaceCoordinate& coordinate) const noexcept
{
    const math::Double3 direction =
        math::Normalize(
            coordinate.unitDirection);

    const auto referenceRadius =
        ReferenceRadiusAlongDirection(
            body,
            direction);

    if (!referenceRadius.has_value() ||
        *referenceRadius <= 0.0)
    {
        return std::nullopt;
    }

    return direction *
        (*referenceRadius +
         coordinate.radialOffsetMeters);
}

std::optional<SurfaceCoordinate>
SurfaceRegistry::SurfaceFromBodyPoint(
    const universe::BodyId body,
    const math::Double3& bodyPoint) const noexcept
{
    const f64 radialDistance =
        math::Length(bodyPoint);

    if (radialDistance <= 0.0)
    {
        return std::nullopt;
    }

    const math::Double3 direction =
        bodyPoint / radialDistance;

    const auto referenceRadius =
        ReferenceRadiusAlongDirection(
            body,
            direction);

    if (!referenceRadius.has_value())
    {
        return std::nullopt;
    }

    return SurfaceCoordinate{
        .unitDirection = direction,
        .radialOffsetMeters =
            radialDistance -
            *referenceRadius
    };
}

void SurfaceRegistry::AttachTerrain(
    const universe::BodyId body,
    std::shared_ptr<const terrain::TerrainSource>
        terrain)
{
    if (terrain == nullptr)
    {
        throw std::invalid_argument(
            "Terrain surface requires a terrain source.");
    }

    if (bodies_.FindBody(body) == nullptr)
    {
        throw std::invalid_argument(
            "Terrain surface body does not exist.");
    }

    // The current Orbit terrain stack is genuinely spherical. Refuse
    // unsupported geometry rather than pretending ellipsoid support.
    if (!SphericalPlanetDefinition(body).
            has_value())
    {
        throw std::invalid_argument(
            "Current terrain surface implementation requires a spherical body.");
    }

    terrainSurfaces_.insert_or_assign(
        body,
        TerrainSurfaceCapability{
            .body = body,
            .terrain =
                std::move(terrain)
        });
}

void SurfaceRegistry::DetachTerrain(
    const universe::BodyId body) noexcept
{
    terrainSurfaces_.erase(body);
}

const TerrainSurfaceCapability*
SurfaceRegistry::FindTerrainSurface(
    const universe::BodyId body) const noexcept
{
    const auto found =
        terrainSurfaces_.find(body);

    return found ==
            terrainSurfaces_.end()
        ? nullptr
        : &found->second;
}
} // namespace orbit::surface
