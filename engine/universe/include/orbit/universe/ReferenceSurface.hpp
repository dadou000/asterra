#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <optional>

namespace orbit::universe
{
// Parametric reference-surface coordinate used by body-local semantic
// systems. Latitude/longitude are radians; offset is metres along the
// reference ellipsoid normal.
struct SurfaceCoordinate
{
    f64 latitudeRadians{0.0};
    f64 longitudeRadians{0.0};
    f64 offsetMeters{0.0};

    [[nodiscard]] constexpr bool operator==(
        const SurfaceCoordinate&) const noexcept = default;
};

[[nodiscard]] math::Double3 ReferenceSurfacePoint(
    const BodyShape& shape,
    const SurfaceCoordinate& coordinate) noexcept;

// Converts a point known to lie on the reference shape into its parametric
// latitude/longitude. The returned coordinate has zero offset.
[[nodiscard]] std::optional<SurfaceCoordinate>
ReferenceSurfaceCoordinate(
    const BodyShape& shape,
    const math::Double3& referencePoint,
    f64 normalizedTolerance = 1.0e-7) noexcept;

// Returns the nearest non-negative intersection in body-local coordinates.
// Direction need not be normalized.
[[nodiscard]] std::optional<math::Double3>
IntersectReferenceSurfaceRay(
    const BodyShape& shape,
    const math::Double3& origin,
    const math::Double3& direction) noexcept;
} // namespace orbit::universe
