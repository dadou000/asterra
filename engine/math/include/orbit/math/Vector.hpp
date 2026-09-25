#pragma once

#include <orbit/core/Types.hpp>

#include <cmath>
#include <initializer_list>
#include <type_traits>

namespace orbit::math
{
template <typename T>
requires std::is_arithmetic_v<T>
struct Vec2
{
    T x{};
    T y{};

    [[nodiscard]] constexpr bool operator==(
        const Vec2&) const noexcept = default;

    [[nodiscard]] constexpr Vec2 operator+(const Vec2& other) const noexcept
    {
        return {x + other.x, y + other.y};
    }

    [[nodiscard]] constexpr Vec2 operator-(const Vec2& other) const noexcept
    {
        return {x - other.x, y - other.y};
    }

    [[nodiscard]] constexpr Vec2 operator*(const T scalar) const noexcept
    {
        return {x * scalar, y * scalar};
    }
};

template <typename T>
requires std::is_arithmetic_v<T>
struct Vec3
{
    T x{};
    T y{};
    T z{};

    [[nodiscard]] constexpr bool operator==(
        const Vec3&) const noexcept = default;

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

    [[nodiscard]] constexpr Vec3 operator/(const T scalar) const noexcept
    {
        return {
            x / scalar,
            y / scalar,
            z / scalar
        };
    }
};

template <typename T>
requires std::is_arithmetic_v<T>
struct Vec4
{
    T x{};
    T y{};
    T z{};
    T w{};

    [[nodiscard]] constexpr bool operator==(
        const Vec4&) const noexcept = default;
};

using Float2 = Vec2<f32>;
using Double2 = Vec2<f64>;
using Float3 = Vec3<f32>;
using Double3 = Vec3<f64>;
using Float4 = Vec4<f32>;
using Double4 = Vec4<f64>;

template <typename T>
[[nodiscard]] constexpr T Dot(
    const Vec3<T>& a,
    const Vec3<T>& b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

template <typename T>
[[nodiscard]] constexpr Vec3<T> Cross(
    const Vec3<T>& a,
    const Vec3<T>& b) noexcept
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

template <typename T>
[[nodiscard]] constexpr T LengthSquared(const Vec3<T>& value) noexcept
{
    return Dot(value, value);
}

[[nodiscard]] inline f32 Length(const Float3& value) noexcept
{
    return std::sqrt(LengthSquared(value));
}

[[nodiscard]] inline f64 Length(const Double3& value) noexcept
{
    return std::sqrt(LengthSquared(value));
}

[[nodiscard]] inline Float3 Normalize(const Float3& value) noexcept
{
    const f32 length = Length(value);

    if (length <= 0.0F)
    {
        return {};
    }

    return value / length;
}

// Makes float brace literals unambiguous when both Float3 and Double3
// normalization overloads are visible. This keeps call sites explicit about
// scalar precision without forcing a temporary variable solely for overload
// resolution.
[[nodiscard]] inline Float3 Normalize(
    const std::initializer_list<f32> values) noexcept
{
    if (values.size() != 3U)
    {
        return {};
    }

    auto it = values.begin();
    const Float3 value{
        *it,
        *std::next(it),
        *std::next(it, 2)
    };

    return Normalize(value);
}

[[nodiscard]] inline Double3 Normalize(const Double3& value) noexcept
{
    const f64 length = Length(value);

    if (length <= 0.0)
    {
        return {};
    }

    return value / length;
}
} // namespace orbit::math
