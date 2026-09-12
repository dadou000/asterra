#include <orbit/terrain/AnalyticTerrainSource.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::terrain
{
namespace
{
[[nodiscard]] u64 Mix64(u64 value) noexcept
{
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] u64 HashLattice(
    const i64 x,
    const i64 y,
    const i64 z,
    const u64 seed) noexcept
{
    u64 hash = seed;
    hash ^= Mix64(static_cast<u64>(x) + 0x632BE59BD9B4E019ULL);
    hash ^= Mix64(static_cast<u64>(y) + 0x8CB92BA72F3D8DD7ULL);
    hash ^= Mix64(static_cast<u64>(z) + 0x58F38DED8C5A935FULL);
    return Mix64(hash);
}

[[nodiscard]] f64 HashValue(
    const i64 x,
    const i64 y,
    const i64 z,
    const u64 seed) noexcept
{
    constexpr f64 inverse53 =
        1.0 / static_cast<f64>(1ULL << 53U);

    const u64 bits = HashLattice(x, y, z, seed) >> 11U;
    return static_cast<f64>(bits) * inverse53 * 2.0 - 1.0;
}

[[nodiscard]] f64 Smooth(const f64 value) noexcept
{
    const f64 x = std::clamp(value, 0.0, 1.0);
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

[[nodiscard]] f64 Lerp(
    const f64 a,
    const f64 b,
    const f64 t) noexcept
{
    return a + (b - a) * t;
}

[[nodiscard]] f64 ValueNoise3D(
    const math::Double3& position,
    const u64 seed) noexcept
{
    const i64 x0 = static_cast<i64>(std::floor(position.x));
    const i64 y0 = static_cast<i64>(std::floor(position.y));
    const i64 z0 = static_cast<i64>(std::floor(position.z));

    const i64 x1 = x0 + 1;
    const i64 y1 = y0 + 1;
    const i64 z1 = z0 + 1;

    const f64 tx = Smooth(position.x - static_cast<f64>(x0));
    const f64 ty = Smooth(position.y - static_cast<f64>(y0));
    const f64 tz = Smooth(position.z - static_cast<f64>(z0));

    const f64 c000 = HashValue(x0, y0, z0, seed);
    const f64 c100 = HashValue(x1, y0, z0, seed);
    const f64 c010 = HashValue(x0, y1, z0, seed);
    const f64 c110 = HashValue(x1, y1, z0, seed);
    const f64 c001 = HashValue(x0, y0, z1, seed);
    const f64 c101 = HashValue(x1, y0, z1, seed);
    const f64 c011 = HashValue(x0, y1, z1, seed);
    const f64 c111 = HashValue(x1, y1, z1, seed);

    const f64 x00 = Lerp(c000, c100, tx);
    const f64 x10 = Lerp(c010, c110, tx);
    const f64 x01 = Lerp(c001, c101, tx);
    const f64 x11 = Lerp(c011, c111, tx);

    const f64 y0v = Lerp(x00, x10, ty);
    const f64 y1v = Lerp(x01, x11, ty);

    return Lerp(y0v, y1v, tz);
}

[[nodiscard]] f64 DetailWeight(
    const f64 wavelengthMeters,
    const f64 footprintMeters) noexcept
{
    if (footprintMeters <= 0.0)
    {
        return 1.0;
    }

    const f64 lower = footprintMeters * 2.0;
    const f64 upper = footprintMeters * 4.0;

    if (wavelengthMeters <= lower)
    {
        return 0.0;
    }

    if (wavelengthMeters >= upper)
    {
        return 1.0;
    }

    return Smooth(
        (wavelengthMeters - lower) / (upper - lower));
}

[[nodiscard]] f64 SampleBand(
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
        planetRadiusMeters / wavelengthMeters;

    return ValueNoise3D(direction * frequency, seed);
}
} // namespace

AnalyticTerrainSource::AnalyticTerrainSource(
    const world::PlanetDefinition planet,
    const AnalyticTerrainDesc desc)
    : planet_(planet),
      desc_(desc)
{
}

TerrainSample AnalyticTerrainSource::Sample(
    const TerrainQuery& query) const noexcept
{
    const math::Double3 direction =
        math::Normalize(query.unitDirection);

    if (math::LengthSquared(direction) <= 0.0)
    {
        return {};
    }

    f64 elevation = 0.0;

    const f64 macroWeight = DetailWeight(
        desc_.macroWavelengthMeters,
        query.footprintMeters);

    elevation +=
        SampleBand(
            direction,
            planet_.radiusMeters,
            desc_.macroWavelengthMeters,
            desc_.seed) *
        desc_.macroAmplitudeMeters *
        macroWeight;

    f64 wavelength = desc_.detailWavelengthMeters;
    f64 amplitude = desc_.detailAmplitudeMeters;

    for (u32 octave = 0;
         octave < desc_.detailOctaves;
         ++octave)
    {
        const f64 weight = DetailWeight(
            wavelength,
            query.footprintMeters);

        if (weight <= 0.0)
        {
            break;
        }

        elevation +=
            SampleBand(
                direction,
                planet_.radiusMeters,
                wavelength,
                desc_.seed + static_cast<u64>(octave + 1U) * 0x9E3779B97F4A7C15ULL) *
            amplitude *
            weight;

        wavelength *= 0.5;
        amplitude *= 0.5;
    }

    return {
        .elevationMeters = elevation
    };
}
} // namespace orbit::terrain
