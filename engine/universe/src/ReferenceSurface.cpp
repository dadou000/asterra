#include <orbit/universe/ReferenceSurface.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace orbit::universe
{
namespace
{
[[nodiscard]] EllipsoidShape AsEllipsoid(
    const BodyShape& shape) noexcept
{
    return std::visit(
        [](const auto& value)
        {
            using Shape =
                std::decay_t<decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Shape,
                    SphereShape>)
            {
                return EllipsoidShape{
                    .radiiMeters = {
                        value.radiusMeters,
                        value.radiusMeters,
                        value.radiusMeters
                    }
                };
            }
            else
            {
                return value;
            }
        },
        shape);
}

[[nodiscard]] bool ValidRadii(
    const math::Double3 radii) noexcept
{
    return
        std::isfinite(radii.x) &&
        std::isfinite(radii.y) &&
        std::isfinite(radii.z) &&
        radii.x > 0.0 &&
        radii.y > 0.0 &&
        radii.z > 0.0;
}
} // namespace

math::Double3 ReferenceSurfacePoint(
    const BodyShape& shape,
    const SurfaceCoordinate& coordinate) noexcept
{
    const EllipsoidShape ellipsoid =
        AsEllipsoid(shape);
    const math::Double3 radii =
        ellipsoid.radiiMeters;

    if (!ValidRadii(radii))
    {
        return {};
    }

    const f64 cosLatitude =
        std::cos(
            coordinate.latitudeRadians);
    const f64 sinLatitude =
        std::sin(
            coordinate.latitudeRadians);
    const f64 cosLongitude =
        std::cos(
            coordinate.longitudeRadians);
    const f64 sinLongitude =
        std::sin(
            coordinate.longitudeRadians);

    math::Double3 point{
        radii.x *
            cosLatitude *
            cosLongitude,
        radii.y *
            sinLatitude,
        radii.z *
            cosLatitude *
            sinLongitude
    };

    math::Double3 normal{
        point.x /
            (radii.x * radii.x),
        point.y /
            (radii.y * radii.y),
        point.z /
            (radii.z * radii.z)
    };

    normal =
        math::Normalize(normal);

    return point +
        normal *
            coordinate.offsetMeters;
}

std::optional<SurfaceCoordinate>
ReferenceSurfaceCoordinate(
    const BodyShape& shape,
    const math::Double3& referencePoint,
    const f64 normalizedTolerance) noexcept
{
    const EllipsoidShape ellipsoid =
        AsEllipsoid(shape);
    const math::Double3 radii =
        ellipsoid.radiiMeters;

    if (!ValidRadii(radii) ||
        normalizedTolerance < 0.0)
    {
        return std::nullopt;
    }

    const math::Double3 scaled{
        referencePoint.x / radii.x,
        referencePoint.y / radii.y,
        referencePoint.z / radii.z
    };

    const f64 scaledLength =
        math::Length(scaled);

    if (!std::isfinite(scaledLength) ||
        scaledLength <=
            std::numeric_limits<f64>::epsilon() ||
        std::abs(scaledLength - 1.0) >
            normalizedTolerance)
    {
        return std::nullopt;
    }

    const math::Double3 direction =
        scaled / scaledLength;

    return SurfaceCoordinate{
        .latitudeRadians =
            std::asin(
                std::clamp(
                    direction.y,
                    -1.0,
                    1.0)),
        .longitudeRadians =
            std::atan2(
                direction.z,
                direction.x),
        .offsetMeters = 0.0
    };
}

std::optional<math::Double3>
IntersectReferenceSurfaceRay(
    const BodyShape& shape,
    const math::Double3& origin,
    const math::Double3& direction) noexcept
{
    const EllipsoidShape ellipsoid =
        AsEllipsoid(shape);
    const math::Double3 radii =
        ellipsoid.radiiMeters;

    if (!ValidRadii(radii))
    {
        return std::nullopt;
    }

    const math::Double3 scaledOrigin{
        origin.x / radii.x,
        origin.y / radii.y,
        origin.z / radii.z
    };
    const math::Double3 scaledDirection{
        direction.x / radii.x,
        direction.y / radii.y,
        direction.z / radii.z
    };

    const f64 a =
        math::Dot(
            scaledDirection,
            scaledDirection);

    if (!std::isfinite(a) ||
        a <= std::numeric_limits<f64>::epsilon())
    {
        return std::nullopt;
    }

    const f64 b =
        2.0 *
        math::Dot(
            scaledOrigin,
            scaledDirection);
    const f64 c =
        math::Dot(
            scaledOrigin,
            scaledOrigin) -
        1.0;
    const f64 discriminant =
        b * b -
        4.0 * a * c;

    if (!std::isfinite(discriminant) ||
        discriminant < 0.0)
    {
        return std::nullopt;
    }

    const f64 root =
        std::sqrt(
            std::max(
                discriminant,
                0.0));
    const f64 denominator =
        2.0 * a;
    const f64 nearT =
        (-b - root) /
        denominator;
    const f64 farT =
        (-b + root) /
        denominator;

    f64 t =
        std::numeric_limits<f64>::
            infinity();

    if (nearT >= 0.0)
    {
        t = nearT;
    }
    else if (farT >= 0.0)
    {
        t = farT;
    }

    if (!std::isfinite(t))
    {
        return std::nullopt;
    }

    return origin +
        direction * t;
}
} // namespace orbit::universe
