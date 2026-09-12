#pragma once

#include <orbit/math/Vector.hpp>

namespace orbit::world
{
struct WorldPosition
{
    math::Double3 meters{};
};

[[nodiscard]] constexpr math::Double3 Difference(
    const WorldPosition& point,
    const WorldPosition& origin) noexcept
{
    return point.meters - origin.meters;
}

[[nodiscard]] constexpr math::Float3 ToCameraRelative(
    const WorldPosition& point,
    const WorldPosition& cameraOrigin) noexcept
{
    const math::Double3 delta = Difference(point, cameraOrigin);

    return {
        static_cast<f32>(delta.x),
        static_cast<f32>(delta.y),
        static_cast<f32>(delta.z)
    };
}

[[nodiscard]] constexpr f64 DistanceSquared(
    const WorldPosition& a,
    const WorldPosition& b) noexcept
{
    return math::LengthSquared(Difference(a, b));
}
} // namespace orbit::world
