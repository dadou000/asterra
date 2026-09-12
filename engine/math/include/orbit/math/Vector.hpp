#pragma once

#include <orbit/core/Types.hpp>

#include <type_traits>

namespace orbit::math
{
template <typename T>
requires std::is_arithmetic_v<T>
struct Vec3
{
    T x{};
    T y{};
    T z{};

    [[nodiscard]] constexpr Vec3 operator+(const Vec3& other) const noexcept
    {
        return {
            x + other.x,
            y + other.y,
            z + other.z
        };
    }

    [[nodiscard]] constexpr Vec3 operator-(const Vec3& other) const noexcept
    {
        return {
            x - other.x,
            y - other.y,
            z - other.z
        };
    }

    [[nodiscard]] constexpr Vec3 operator*(const T scalar) const noexcept
    {
        return {
            x * scalar,
            y * scalar,
            z * scalar
        };
    }
};

using Float3 = Vec3<f32>;
using Double3 = Vec3<f64>;

[[nodiscard]] constexpr f64 LengthSquared(const Double3& value) noexcept
{
    return
        value.x * value.x +
        value.y * value.y +
        value.z * value.z;
}
} // namespace orbit::math
