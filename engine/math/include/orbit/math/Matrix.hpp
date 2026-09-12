#pragma once

#include <orbit/math/Vector.hpp>

#include <array>
#include <cmath>

namespace orbit::math
{
struct Mat4
{
    std::array<f32, 16> values{};

    [[nodiscard]] constexpr f32& At(
        const u32 row,
        const u32 column) noexcept
    {
        return values[
            static_cast<std::size_t>(row) * 4U +
            static_cast<std::size_t>(column)];
    }

    [[nodiscard]] constexpr const f32& At(
        const u32 row,
        const u32 column) const noexcept
    {
        return values[
            static_cast<std::size_t>(row) * 4U +
            static_cast<std::size_t>(column)];
    }
};

[[nodiscard]] constexpr Mat4 Identity4() noexcept
{
    Mat4 result{};
    result.At(0, 0) = 1.0F;
    result.At(1, 1) = 1.0F;
    result.At(2, 2) = 1.0F;
    result.At(3, 3) = 1.0F;
    return result;
}

[[nodiscard]] constexpr Mat4 Multiply(
    const Mat4& a,
    const Mat4& b) noexcept
{
    Mat4 result{};

    for (u32 row = 0; row < 4; ++row)
    {
        for (u32 column = 0; column < 4; ++column)
        {
            f32 sum = 0.0F;

            for (u32 k = 0; k < 4; ++k)
            {
                sum += a.At(row, k) * b.At(k, column);
            }

            result.At(row, column) = sum;
        }
    }

    return result;
}

[[nodiscard]] inline Mat4 PerspectiveLH(
    const f32 verticalFovRadians,
    const f32 aspectRatio,
    const f32 nearPlane,
    const f32 farPlane) noexcept
{
    Mat4 result{};

    if (verticalFovRadians <= 0.0F ||
        aspectRatio <= 0.0F ||
        nearPlane <= 0.0F ||
        farPlane <= nearPlane)
    {
        return result;
    }

    const f32 yScale =
        1.0F / std::tan(verticalFovRadians * 0.5F);
    const f32 xScale = yScale / aspectRatio;
    const f32 zScale =
        farPlane / (farPlane - nearPlane);

    result.At(0, 0) = xScale;
    result.At(1, 1) = yScale;
    result.At(2, 2) = zScale;
    result.At(2, 3) = 1.0F;
    result.At(3, 2) = -nearPlane * zScale;

    return result;
}

[[nodiscard]] inline Mat4 PerspectiveReverseZLH(
    const f32 verticalFovRadians,
    const f32 aspectRatio,
    const f32 nearPlane,
    const f32 farPlane) noexcept
{
    Mat4 result{};

    if (verticalFovRadians <= 0.0F ||
        aspectRatio <= 0.0F ||
        nearPlane <= 0.0F ||
        farPlane <= nearPlane)
    {
        return result;
    }

    const f32 yScale =
        1.0F / std::tan(verticalFovRadians * 0.5F);
    const f32 xScale = yScale / aspectRatio;

    // D3D depth range [0, 1], reversed so near -> 1 and far -> 0.
    // Keeping this finite rather than infinite preserves an explicit
    // culling horizon while retaining float-depth precision near the eye.
    const f32 inverseRange =
        1.0F / (farPlane - nearPlane);

    result.At(0, 0) = xScale;
    result.At(1, 1) = yScale;
    result.At(2, 2) = -nearPlane * inverseRange;
    result.At(2, 3) = 1.0F;
    result.At(3, 2) =
        nearPlane *
        farPlane *
        inverseRange;

    return result;
}

[[nodiscard]] inline Mat4 LookAtLH(
    const Float3& eye,
    const Float3& target,
    const Float3& upHint) noexcept
{
    const Float3 forward =
        Normalize(target - eye);

    const Float3 right =
        Normalize(Cross(upHint, forward));

    const Float3 up =
        Cross(forward, right);

    Mat4 result = Identity4();

    result.At(0, 0) = right.x;
    result.At(1, 0) = right.y;
    result.At(2, 0) = right.z;

    result.At(0, 1) = up.x;
    result.At(1, 1) = up.y;
    result.At(2, 1) = up.z;

    result.At(0, 2) = forward.x;
    result.At(1, 2) = forward.y;
    result.At(2, 2) = forward.z;

    result.At(3, 0) = -Dot(right, eye);
    result.At(3, 1) = -Dot(up, eye);
    result.At(3, 2) = -Dot(forward, eye);

    return result;
}
} // namespace orbit::math
