#include <orbit/celestial_stellar/StellarAppearance.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace orbit::celestial_stellar
{
namespace
{
void Validate(const StellarAppearanceParameters& p)
{
    if (!std::isfinite(p.effectiveTemperatureKelvin) ||
        p.effectiveTemperatureKelvin <= 0.0 ||
        !std::isfinite(p.limbDarkening) ||
        p.limbDarkening < 0.0 || p.limbDarkening > 1.0 ||
        !std::isfinite(p.granulationStrength) ||
        p.granulationStrength < 0.0 || p.granulationStrength > 1.0 ||
        !std::isfinite(p.granulationScale) ||
        p.granulationScale < 1.0 ||
        !std::isfinite(p.activityLevel) ||
        p.activityLevel < 0.0 || p.activityLevel > 1.0 ||
        !std::isfinite(p.chromosphereStrength) ||
        p.chromosphereStrength < 0.0 ||
        !std::isfinite(p.chromosphereExtent) ||
        p.chromosphereExtent < 0.0 ||
        !std::isfinite(p.coronaStrength) ||
        p.coronaStrength < 0.0 ||
        !std::isfinite(p.coronaExtent) ||
        p.coronaExtent < 0.0 ||
        !std::isfinite(p.glareStrength) ||
        p.glareStrength < 0.0 ||
        !std::isfinite(p.glareRadiusPixels) ||
        p.glareRadiusPixels < 0.5)
    {
        throw std::invalid_argument(
            "Stellar appearance parameters are invalid.");
    }
}

[[nodiscard]] u64 Mix(u64 x) noexcept
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31U);
}

[[nodiscard]] f64 Hash3(
    const math::Double3 p,
    const f64 scale,
    const u64 seed) noexcept
{
    const i64 x = static_cast<i64>(
        std::floor(p.x * scale * 4096.0));
    const i64 y = static_cast<i64>(
        std::floor(p.y * scale * 4096.0));
    const i64 z = static_cast<i64>(
        std::floor(p.z * scale * 4096.0));

    u64 h = Mix(seed ^ static_cast<u64>(x));
    h = Mix(h ^ static_cast<u64>(y));
    h = Mix(h ^ static_cast<u64>(z));

    return static_cast<f64>(
        h & 0x00ffffffULL) /
        static_cast<f64>(0x00ffffffULL);
}

[[nodiscard]] f64 Fractal(
    const math::Double3 p,
    const f64 scale,
    const u64 seed) noexcept
{
    f64 sum = 0.0;
    f64 amplitude = 1.0;
    f64 weight = 0.0;
    f64 frequency = scale;

    for (u32 octave = 0U;
         octave < 4U;
         ++octave)
    {
        sum +=
            Hash3(
                p,
                frequency,
                seed +
                    static_cast<u64>(octave) *
                        0x9e3779b9ULL) *
            amplitude;
        weight += amplitude;
        amplitude *= 0.5;
        frequency *= 2.07;
    }

    return weight > 0.0
        ? sum / weight
        : 0.5;
}
} // namespace

math::Double3 BlackbodyColorLinear(
    const f64 temperatureKelvin)
{
    if (!std::isfinite(temperatureKelvin) ||
        temperatureKelvin <= 0.0)
    {
        throw std::invalid_argument(
            "Blackbody color requires a finite positive temperature.");
    }

    // CIE xy approximation of the Planckian locus; clamp only the
    // chromaticity approximation range, not the M19 physical temperature.
    const f64 t =
        std::clamp(
            temperatureKelvin,
            1667.0,
            25000.0);

    const f64 t2 = t * t;
    const f64 t3 = t2 * t;

    const f64 x =
        t <= 4000.0
            ? -0.2661239e9 / t3 -
                  0.2343580e6 / t2 +
                  0.8776956e3 / t +
                  0.179910
            : -3.0258469e9 / t3 +
                  2.1070379e6 / t2 +
                  0.2226347e3 / t +
                  0.240390;

    f64 y = 0.0;

    if (t <= 2222.0)
    {
        y =
            -1.1063814 * x * x * x -
            1.34811020 * x * x +
            2.18555832 * x -
            0.20219683;
    }
    else if (t <= 4000.0)
    {
        y =
            -0.9549476 * x * x * x -
            1.37418593 * x * x +
            2.09137015 * x -
            0.16748867;
    }
    else
    {
        y =
            3.0817580 * x * x * x -
            5.87338670 * x * x +
            3.75112997 * x -
            0.37001483;
    }

    const f64 safeY =
        std::max(y, 1.0e-6);

    const f64 X = x / safeY;
    const f64 Y = 1.0;
    const f64 Z =
        std::max(
            (1.0 - x - y) /
                safeY,
            0.0);

    math::Double3 rgb{
        3.2404542 * X -
            1.5371385 * Y -
            0.4985314 * Z,
       -0.9692660 * X +
            1.8760108 * Y +
            0.0415560 * Z,
        0.0556434 * X -
            0.2040259 * Y +
            1.0572252 * Z
    };

    rgb.x = std::max(rgb.x, 0.0);
    rgb.y = std::max(rgb.y, 0.0);
    rgb.z = std::max(rgb.z, 0.0);

    const f64 maximum =
        std::max({
            rgb.x,
            rgb.y,
            rgb.z,
            1.0e-9
        });

    return rgb / maximum;
}

f64 LinearLimbDarkening(
    const f64 cosineEmissionAngle,
    const f64 coefficient)
{
    if (!std::isfinite(coefficient) ||
        coefficient < 0.0 ||
        coefficient > 1.0)
    {
        throw std::invalid_argument(
            "Limb-darkening coefficient must be in [0,1].");
    }

    const f64 mu =
        std::clamp(
            cosineEmissionAngle,
            0.0,
            1.0);

    return
        1.0 -
        coefficient *
            (1.0 - mu);
}

f64 GranulationModulation(
    math::Double3 direction,
    const StellarAppearanceParameters& p) noexcept
{
    if (math::LengthSquared(direction) <= 1.0e-20)
        return 1.0;

    direction = math::Normalize(direction);

    const f64 noise =
        Fractal(
            direction,
            std::max(
                p.granulationScale,
                1.0),
            p.activitySeed ^
                0x4752414e554c4152ULL);

    const f64 centered =
        (noise - 0.5) * 2.0;

    return std::max(
        0.0,
        1.0 +
            centered *
                std::clamp(
                    p.granulationStrength,
                    0.0,
                    1.0));
}

f64 ActivityModulation(
    math::Double3 direction,
    const StellarAppearanceParameters& p) noexcept
{
    if (math::LengthSquared(direction) <= 1.0e-20)
        return 1.0;

    direction = math::Normalize(direction);

    const f64 large =
        Fractal(
            direction,
            3.7,
            p.activitySeed ^
                0x5354415241435449ULL);

    const f64 spotThreshold =
        1.0 -
        0.10 *
            std::clamp(
                p.activityLevel,
                0.0,
                1.0);

    if (large > spotThreshold)
    {
        const f64 spot =
            (large - spotThreshold) /
            std::max(
                1.0 - spotThreshold,
                1.0e-6);

        return
            1.0 -
            0.55 *
                std::clamp(
                    spot,
                    0.0,
                    1.0);
    }

    const f64 faculae =
        std::max(
            0.0,
            large -
                (spotThreshold - 0.08));

    return
        1.0 +
        faculae *
            0.35 *
            std::clamp(
                p.activityLevel,
                0.0,
                1.0);
}

f64 ChromosphereProfile(
    const f64 normalizedRadius,
    const StellarAppearanceParameters& p) noexcept
{
    if (normalizedRadius < 1.0 ||
        p.chromosphereExtent <= 0.0)
    {
        return 0.0;
    }

    const f64 x =
        (normalizedRadius - 1.0) /
        std::max(
            p.chromosphereExtent,
            1.0e-6);

    return x <= 1.0
        ? p.chromosphereStrength *
              std::exp(-4.0 * x)
        : 0.0;
}

f64 CoronaProfile(
    const f64 normalizedRadius,
    const StellarAppearanceParameters& p) noexcept
{
    if (normalizedRadius < 1.0 ||
        p.coronaExtent <= 0.0)
    {
        return 0.0;
    }

    const f64 x =
        (normalizedRadius - 1.0) /
        std::max(
            p.coronaExtent,
            1.0e-6);

    if (x > 1.0)
        return 0.0;

    return
        p.coronaStrength /
        std::pow(
            1.0 +
                7.0 * x,
            2.25);
}

u64 StellarAppearanceFingerprint(
    const StellarAppearanceParameters& p)
{
    Validate(p);

    u64 h =
        0x4d32365354454c4cULL;

    const auto add =
        [&h](const u64 v)
        {
            h =
                terrain::StableCombine64(
                    h,
                    v);
        };

    const auto addf =
        [&add](const f64 v)
        {
            add(std::bit_cast<u64>(v));
        };

    addf(p.effectiveTemperatureKelvin);
    addf(p.limbDarkening);
    addf(p.granulationStrength);
    addf(p.granulationScale);
    addf(p.activityLevel);
    add(p.activitySeed);
    addf(p.chromosphereStrength);
    addf(p.chromosphereExtent);
    addf(p.coronaStrength);
    addf(p.coronaExtent);
    addf(p.glareStrength);
    addf(p.glareRadiusPixels);

    return h;
}
} // namespace orbit::celestial_stellar
