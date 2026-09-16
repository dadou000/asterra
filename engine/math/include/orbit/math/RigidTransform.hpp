#pragma once

#include <orbit/math/Vector.hpp>

namespace orbit::math
{
struct Double3x3
{
    // Column vectors: the local X/Y/Z axes expressed in the parent frame.
    Double3 xAxis{1.0, 0.0, 0.0};
    Double3 yAxis{0.0, 1.0, 0.0};
    Double3 zAxis{0.0, 0.0, 1.0};

    [[nodiscard]] constexpr bool operator==(
        const Double3x3&) const noexcept = default;
};

[[nodiscard]] constexpr Double3x3 Identity3D() noexcept
{
    return {};
}

[[nodiscard]] constexpr Double3 TransformVector(
    const Double3x3& rotation,
    const Double3& value) noexcept
{
    return rotation.xAxis * value.x +
        rotation.yAxis * value.y +
        rotation.zAxis * value.z;
}

[[nodiscard]] constexpr Double3x3 Transpose(
    const Double3x3& value) noexcept
{
    return {
        .xAxis = {
            value.xAxis.x,
            value.yAxis.x,
            value.zAxis.x
        },
        .yAxis = {
            value.xAxis.y,
            value.yAxis.y,
            value.zAxis.y
        },
        .zAxis = {
            value.xAxis.z,
            value.yAxis.z,
            value.zAxis.z
        }
    };
}

[[nodiscard]] constexpr Double3x3 Multiply(
    const Double3x3& lhs,
    const Double3x3& rhs) noexcept
{
    return {
        .xAxis =
            TransformVector(lhs, rhs.xAxis),
        .yAxis =
            TransformVector(lhs, rhs.yAxis),
        .zAxis =
            TransformVector(lhs, rhs.zAxis)
    };
}

struct RigidTransformD
{
    Double3x3 rotation{};
    Double3 translation{};

    [[nodiscard]] constexpr bool operator==(
        const RigidTransformD&) const noexcept = default;
};

[[nodiscard]] constexpr RigidTransformD
IdentityRigidTransformD() noexcept
{
    return {};
}

[[nodiscard]] constexpr Double3 TransformPoint(
    const RigidTransformD& transform,
    const Double3& point) noexcept
{
    return TransformVector(
               transform.rotation,
               point) +
        transform.translation;
}

// Returns lhs * rhs: a point is first transformed by rhs, then lhs.
[[nodiscard]] constexpr RigidTransformD Compose(
    const RigidTransformD& lhs,
    const RigidTransformD& rhs) noexcept
{
    return {
        .rotation =
            Multiply(
                lhs.rotation,
                rhs.rotation),
        .translation =
            TransformPoint(
                lhs,
                rhs.translation)
    };
}

[[nodiscard]] constexpr RigidTransformD Inverse(
    const RigidTransformD& transform) noexcept
{
    const Double3x3 inverseRotation =
        Transpose(transform.rotation);

    return {
        .rotation = inverseRotation,
        .translation =
            TransformVector(
                inverseRotation,
                transform.translation *
                    -1.0)
    };
}
} // namespace orbit::math
