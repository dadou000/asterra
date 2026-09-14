#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace orbit::terrain::detail
{
[[nodiscard]] inline u64 Mix64(
    u64 value) noexcept
{
    value += 0x9E3779B97F4A7C15ULL;
    value =
        (value ^
         (value >> 30U)) *
        0xBF58476D1CE4E5B9ULL;

    value =
        (value ^
         (value >> 27U)) *
        0x94D049BB133111EBULL;

    return value ^
        (value >> 31U);
}

[[nodiscard]] inline u64 HashLattice(
    const i64 x,
    const i64 y,
    const i64 z,
    const u64 seed) noexcept
{
    u64 hash = seed;

    hash ^=
        Mix64(
            static_cast<u64>(x) +
            0x632BE59BD9B4E019ULL);

    hash ^=
        Mix64(
            static_cast<u64>(y) +
            0x8CB92BA72F3D8DD7ULL);

    hash ^=
        Mix64(
            static_cast<u64>(z) +
            0x58F38DED8C5A935FULL);

    return Mix64(hash);
}

[[nodiscard]] inline f64 HashValue(
    const i64 x,
    const i64 y,
    const i64 z,
    const u64 seed) noexcept
{
    constexpr f64 inverse53 =
        1.0 /
        static_cast<f64>(
            1ULL << 53U);

    const u64 bits =
        HashLattice(
            x,
            y,
            z,
            seed) >>
        11U;

    return
        static_cast<f64>(bits) *
            inverse53 *
            2.0 -
        1.0;
}

[[nodiscard]] inline f64 Smooth(
    const f64 value) noexcept
{
    const f64 x =
        std::clamp(
            value,
            0.0,
            1.0);

    return
        x * x * x *
        (x *
             (x * 6.0 - 15.0) +
         10.0);
}

[[nodiscard]] inline f64 Lerp(
    const f64 a,
    const f64 b,
    const f64 t) noexcept
{
    return a +
        (b - a) *
            t;
}

struct NoiseCell
{
    std::array<u64, 8> hashes;
    math::Double3 blend;
};

[[nodiscard]] inline NoiseCell MakeNoiseCell(
    const math::Double3& position,
    const u64 seed) noexcept
{
    const i64 x = static_cast<i64>(std::floor(position.x));
    const i64 y = static_cast<i64>(std::floor(position.y));
    const i64 z = static_cast<i64>(std::floor(position.z));
    // Six axis hashes serve all eight corners (formerly 24 axis mixes).
    // Unsigned arithmetic also defines wraparound at negative coordinates.
    const u64 x0 = Mix64(static_cast<u64>(x) + 0x632BE59BD9B4E019ULL);
    const u64 x1 = Mix64(static_cast<u64>(x) + 1U + 0x632BE59BD9B4E019ULL);
    const u64 y0 = Mix64(static_cast<u64>(y) + 0x8CB92BA72F3D8DD7ULL);
    const u64 y1 = Mix64(static_cast<u64>(y) + 1U + 0x8CB92BA72F3D8DD7ULL);
    const u64 z0 = Mix64(static_cast<u64>(z) + 0x58F38DED8C5A935FULL);
    const u64 z1 = Mix64(static_cast<u64>(z) + 1U + 0x58F38DED8C5A935FULL);
    return {
        .hashes = {
            Mix64(seed ^ x0 ^ y0 ^ z0), Mix64(seed ^ x1 ^ y0 ^ z0),
            Mix64(seed ^ x0 ^ y1 ^ z0), Mix64(seed ^ x1 ^ y1 ^ z0),
            Mix64(seed ^ x0 ^ y0 ^ z1), Mix64(seed ^ x1 ^ y0 ^ z1),
            Mix64(seed ^ x0 ^ y1 ^ z1), Mix64(seed ^ x1 ^ y1 ^ z1)
        },
        .blend = {
            Smooth(position.x - static_cast<f64>(x)),
            Smooth(position.y - static_cast<f64>(y)),
            Smooth(position.z - static_cast<f64>(z))
        }
    };
}

template <typename T>
[[nodiscard]] inline T InterpolateNoiseCell(
    const std::array<T, 8>& values,
    const math::Double3& blend) noexcept
{
    const auto lerp = [](const T& a, const T& b, const f64 t) { return a + (b - a) * t; };
    return lerp(
        lerp(lerp(values[0], values[1], blend.x), lerp(values[2], values[3], blend.x), blend.y),
        lerp(lerp(values[4], values[5], blend.x), lerp(values[6], values[7], blend.x), blend.y),
        blend.z);
}

[[nodiscard]] inline f64 ValueNoise3D(
    const math::Double3& position,
    const u64 seed) noexcept
{
    const auto cell = MakeNoiseCell(position, seed);
    std::array<f64, 8> values;
    constexpr f64 inverse53 = 1.0 / static_cast<f64>(1ULL << 53U);
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        values[i] = static_cast<f64>(cell.hashes[i] >> 11U) * inverse53 * 2.0 - 1.0;
    }
    return InterpolateNoiseCell(values, cell.blend);
}

[[nodiscard]] inline math::Double3 VectorNoise3D(
    const math::Double3& position,
    const u64 seed) noexcept
{
    const auto cell = MakeNoiseCell(position, seed);
    std::array<math::Double3, 8> values;
    constexpr u64 mask = (1ULL << 21U) - 1U;
    constexpr f64 scale = 2.0 / static_cast<f64>(mask);
    // Three disjoint 21-bit channels share lattice hashing, floor and fade.
    // This is deterministic vector noise, not three full scalar noise calls.
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        const u64 bits = cell.hashes[i];
        values[i] = {
            static_cast<f64>(bits & mask) * scale - 1.0,
            static_cast<f64>((bits >> 21U) & mask) * scale - 1.0,
            static_cast<f64>((bits >> 42U) & mask) * scale - 1.0
        };
    }
    return InterpolateNoiseCell(values, cell.blend);
}

[[nodiscard]] inline f64 DetailWeight(
    const f64 wavelengthMeters,
    const f64 footprintMeters) noexcept
{
    if (footprintMeters <= 0.0)
    {
        return 1.0;
    }

    const f64 lower =
        footprintMeters *
        2.0;

    const f64 upper =
        footprintMeters *
        4.0;

    if (wavelengthMeters <=
        lower)
    {
        return 0.0;
    }

    if (wavelengthMeters >=
        upper)
    {
        return 1.0;
    }

    return Smooth(
        (wavelengthMeters -
         lower) /
        (upper - lower));
}

[[nodiscard]] inline f64 SampleBand(
    const math::Double3& direction,
    const f64 planetRadiusMeters,
    const f64 wavelengthMeters,
    const u64 seed) noexcept
{
    if (wavelengthMeters <= 0.0 ||
        planetRadiusMeters <= 0.0)
    {
        return 0.0;
    }

    const f64 frequency =
        planetRadiusMeters /
        wavelengthMeters;

    return ValueNoise3D(
        direction * frequency,
        seed);
}

[[nodiscard]] inline f64 RidgedBand(
    const math::Double3& direction,
    const f64 planetRadiusMeters,
    const f64 wavelengthMeters,
    const u64 seed) noexcept
{
    const f64 noise =
        SampleBand(
            direction,
            planetRadiusMeters,
            wavelengthMeters,
            seed);

    const f64 ridge =
        1.0 -
        std::abs(noise);

    return ridge *
        ridge *
        ridge;
}
} // namespace orbit::terrain::detail
