#include <orbit/celestial_atmosphere/Atmosphere.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace orbit::celestial_atmosphere
{
namespace
{
[[nodiscard]] u64 Combine(
    u64 seed,
    const u64 value) noexcept
{
    seed ^=
        value +
        0x9e3779b97f4a7c15ULL +
        (seed << 6U) +
        (seed >> 2U);
    return seed;
}

[[nodiscard]] u64 HashDouble(
    const f64 value) noexcept
{
    return std::bit_cast<u64>(value);
}

[[nodiscard]] u64 HashDouble3(
    const math::Double3 value) noexcept
{
    u64 seed = 0x41544d4f53504831ULL;
    seed = Combine(seed, HashDouble(value.x));
    seed = Combine(seed, HashDouble(value.y));
    seed = Combine(seed, HashDouble(value.z));
    return seed;
}

void Validate(
    const AtmosphereParameters& p,
    const AtmosphereLutConfig& c)
{
    const auto positive =
        [](const f64 value)
        {
            return
                std::isfinite(value) &&
                value > 0.0;
        };

    if (!positive(p.bottomRadiusMeters) ||
        !positive(p.topRadiusMeters) ||
        p.topRadiusMeters <=
            p.bottomRadiusMeters ||
        !positive(
            p.rayleighScaleHeightMeters) ||
        !positive(
            p.mieScaleHeightMeters) ||
        !positive(
            p.absorptionHalfWidthMeters) ||
        !std::isfinite(p.mieAnisotropy) ||
        p.mieAnisotropy <= -1.0 ||
        p.mieAnisotropy >= 1.0 ||
        c.transmittanceWidth < 2U ||
        c.transmittanceHeight < 2U ||
        c.multiScatteringWidth < 2U ||
        c.multiScatteringHeight < 2U ||
        c.skyViewWidth < 2U ||
        c.skyViewHeight < 2U ||
        c.opticalDepthSteps < 4U ||
        c.multiDirectionSamples < 8U ||
        c.skyViewSteps < 4U)
    {
        throw std::invalid_argument(
            "Atmosphere parameters/config are invalid.");
    }

    const auto nonNegative3 =
        [](const math::Double3 v)
        {
            return
                std::isfinite(v.x) &&
                std::isfinite(v.y) &&
                std::isfinite(v.z) &&
                v.x >= 0.0 &&
                v.y >= 0.0 &&
                v.z >= 0.0;
        };

    if (!nonNegative3(
            p.rayleighScatteringPerMeter) ||
        !nonNegative3(
            p.mieScatteringPerMeter) ||
        !nonNegative3(
            p.mieExtinctionPerMeter) ||
        !nonNegative3(
            p.absorptionExtinctionPerMeter) ||
        !nonNegative3(
            p.groundAlbedo))
    {
        throw std::invalid_argument(
            "Atmosphere coefficients must be finite and non-negative.");
    }
}

[[nodiscard]] math::Double3 ExpNeg(
    const math::Double3 v) noexcept
{
    return {
        std::exp(-v.x),
        std::exp(-v.y),
        std::exp(-v.z)
    };
}

[[nodiscard]] math::Double3 Hadamard(
    const math::Double3 a,
    const math::Double3 b) noexcept
{
    return {
        a.x * b.x,
        a.y * b.y,
        a.z * b.z
    };
}

[[nodiscard]] math::Double3 DivideSafe(
    const math::Double3 a,
    const math::Double3 b) noexcept
{
    return {
        b.x > 1.0e-20 ? a.x / b.x : 0.0,
        b.y > 1.0e-20 ? a.y / b.y : 0.0,
        b.z > 1.0e-20 ? a.z / b.z : 0.0
    };
}

[[nodiscard]] math::Double3 Clamp01(
    const math::Double3 v) noexcept
{
    return {
        std::clamp(v.x, 0.0, 1.0),
        std::clamp(v.y, 0.0, 1.0),
        std::clamp(v.z, 0.0, 1.0)
    };
}

[[nodiscard]] f64 PositiveRaySphereDistance(
    const math::Double3 origin,
    const math::Double3 direction,
    const f64 radius)
{
    const f64 b =
        math::Dot(
            origin,
            direction);
    const f64 c =
        math::Dot(origin, origin) -
        radius * radius;
    const f64 discriminant =
        b * b - c;

    if (discriminant < 0.0)
    {
        return
            std::numeric_limits<f64>::
                infinity();
    }

    const f64 root =
        std::sqrt(
            std::max(
                discriminant,
                0.0));

    const f64 t0 =
        -b - root;
    const f64 t1 =
        -b + root;

    if (t0 > 1.0e-9)
    {
        return t0;
    }

    if (t1 > 1.0e-9)
    {
        return t1;
    }

    return
        std::numeric_limits<f64>::
            infinity();
}

struct RaySphereInterval
{
    f64 nearMeters{
        std::numeric_limits<f64>::
            infinity()};
    f64 farMeters{
        std::numeric_limits<f64>::
            infinity()};
    bool intersects{false};
};

[[nodiscard]] RaySphereInterval RaySphereIntervalFor(
    const math::Double3 origin,
    const math::Double3 direction,
    const f64 radius)
{
    const f64 b =
        math::Dot(
            origin,
            direction);
    const f64 c =
        math::Dot(origin, origin) -
        radius * radius;
    const f64 discriminant =
        b * b - c;

    if (discriminant < 0.0)
    {
        return {};
    }

    const f64 root =
        std::sqrt(
            std::max(
                discriminant,
                0.0));

    return {
        .nearMeters =
            -b - root,
        .farMeters =
            -b + root,
        .intersects = true
    };
}

struct RayBoundary
{
    f64 distanceMeters{0.0};
    bool groundHit{false};
};

[[nodiscard]] RayBoundary BoundaryDistance(
    const AtmosphereParameters& p,
    const math::Double3 origin,
    const math::Double3 direction)
{
    const f64 top =
        PositiveRaySphereDistance(
            origin,
            direction,
            p.topRadiusMeters);

    const f64 ground =
        PositiveRaySphereDistance(
            origin,
            direction,
            p.bottomRadiusMeters);

    if (ground < top)
    {
        return {
            .distanceMeters = ground,
            .groundHit = true
        };
    }

    return {
        .distanceMeters = top,
        .groundHit = false
    };
}

[[nodiscard]] math::Double3 OpticalDepth(
    const AtmosphereParameters& p,
    const math::Double3 origin,
    const math::Double3 direction,
    const f64 distanceMeters,
    const u32 steps)
{
    if (!std::isfinite(distanceMeters) ||
        distanceMeters <= 0.0)
    {
        return {};
    }

    const f64 step =
        distanceMeters /
        static_cast<f64>(steps);

    math::Double3 opticalDepth{};

    for (u32 i = 0U;
         i <= steps;
         ++i)
    {
        const f64 t =
            step *
            static_cast<f64>(i);

        const auto sample =
            SampleAtmosphere(
                p,
                math::Length(
                    origin +
                    direction * t));

        const f64 weight =
            (i == 0U || i == steps)
                ? 0.5
                : 1.0;

        opticalDepth =
            opticalDepth +
            sample.extinctionPerMeter *
                (step * weight);
    }

    return opticalDepth;
}

[[nodiscard]] math::Double3 TransmittanceAlong(
    const AtmosphereParameters& p,
    const math::Double3 origin,
    const math::Double3 direction,
    const u32 steps,
    const bool blockOnGround)
{
    const auto boundary =
        BoundaryDistance(
            p,
            origin,
            direction);

    if (!std::isfinite(
            boundary.distanceMeters))
    {
        return {};
    }

    if (blockOnGround &&
        boundary.groundHit)
    {
        return {};
    }

    return ExpNeg(
        OpticalDepth(
            p,
            origin,
            direction,
            boundary.distanceMeters,
            steps));
}

[[nodiscard]] f64 RadiusFromUnit(
    const AtmosphereParameters& p,
    const f64 unit) noexcept
{
    const f64 u =
        std::clamp(
            unit,
            0.0,
            1.0);

    // sqrt mapping allocates more resolution near the dense lower atmosphere.
    return
        std::sqrt(
            p.bottomRadiusMeters *
                p.bottomRadiusMeters +
            u *
                (p.topRadiusMeters *
                     p.topRadiusMeters -
                 p.bottomRadiusMeters *
                     p.bottomRadiusMeters));
}

[[nodiscard]] f64 UnitFromRadius(
    const AtmosphereParameters& p,
    const f64 radius) noexcept
{
    const f64 denominator =
        p.topRadiusMeters *
            p.topRadiusMeters -
        p.bottomRadiusMeters *
            p.bottomRadiusMeters;

    if (denominator <= 0.0)
    {
        return 0.0;
    }

    return std::clamp(
        (radius * radius -
         p.bottomRadiusMeters *
             p.bottomRadiusMeters) /
            denominator,
        0.0,
        1.0);
}

[[nodiscard]] math::Double3 LutRgb(
    const AtmosphereLut2D& lut,
    const f64 u,
    const f64 v)
{
    const f64 fx =
        std::clamp(u, 0.0, 1.0) *
        static_cast<f64>(
            lut.width - 1U);
    const f64 fy =
        std::clamp(v, 0.0, 1.0) *
        static_cast<f64>(
            lut.height - 1U);

    const u32 x0 =
        static_cast<u32>(
            std::floor(fx));
    const u32 y0 =
        static_cast<u32>(
            std::floor(fy));
    const u32 x1 =
        std::min(
            x0 + 1U,
            lut.width - 1U);
    const u32 y1 =
        std::min(
            y0 + 1U,
            lut.height - 1U);

    const f64 tx =
        fx -
        static_cast<f64>(x0);
    const f64 ty =
        fy -
        static_cast<f64>(y0);

    const auto convert =
        [](const math::Float4 value)
        {
            return math::Double3{
                value.x,
                value.y,
                value.z
            };
        };

    const auto a =
        convert(lut.At(x0, y0));
    const auto b =
        convert(lut.At(x1, y0));
    const auto c =
        convert(lut.At(x0, y1));
    const auto d =
        convert(lut.At(x1, y1));

    return
        (a * (1.0 - tx) +
         b * tx) *
            (1.0 - ty) +
        (c * (1.0 - tx) +
         d * tx) *
            ty;
}

[[nodiscard]] f64 RayleighPhase(
    const f64 cosine) noexcept
{
    return
        3.0 /
        (16.0 *
         std::numbers::pi_v<f64>) *
        (1.0 + cosine * cosine);
}

[[nodiscard]] f64 MiePhase(
    const f64 cosine,
    const f64 g) noexcept
{
    const f64 gg =
        g * g;
    const f64 denominator =
        1.0 + gg -
        2.0 * g * cosine;

    return
        (1.0 - gg) /
        (4.0 *
         std::numbers::pi_v<f64> *
         std::pow(
             std::max(
                 denominator,
                 1.0e-8),
             1.5));
}

[[nodiscard]] u16 FloatToHalf(
    const f32 value) noexcept
{
    const u32 bits =
        std::bit_cast<u32>(value);
    const u32 sign =
        (bits >> 16U) &
        0x8000U;
    const i32 exponent =
        static_cast<i32>(
            (bits >> 23U) &
            0xffU) -
        127 +
        15;
    u32 mantissa =
        bits &
        0x7fffffU;

    if (exponent <= 0)
    {
        if (exponent < -10)
        {
            return
                static_cast<u16>(sign);
        }

        mantissa |=
            0x800000U;

        const u32 shift =
            static_cast<u32>(
                14 - exponent);

        u32 halfMantissa =
            mantissa >> shift;

        if ((mantissa >>
             (shift - 1U)) &
            1U)
        {
            ++halfMantissa;
        }

        return static_cast<u16>(
            sign |
            halfMantissa);
    }

    if (exponent >= 31)
    {
        return static_cast<u16>(
            sign | 0x7c00U);
    }

    u32 halfMantissa =
        mantissa >> 13U;

    if (mantissa &
        0x1000U)
    {
        ++halfMantissa;

        if (halfMantissa &
            0x0400U)
        {
            halfMantissa = 0U;
            const i32 nextExponent =
                exponent + 1;

            if (nextExponent >= 31)
            {
                return static_cast<u16>(
                    sign | 0x7c00U);
            }

            return static_cast<u16>(
                sign |
                (static_cast<u32>(
                     nextExponent)
                 << 10U));
        }
    }

    return static_cast<u16>(
        sign |
        (static_cast<u32>(exponent)
         << 10U) |
        (halfMantissa &
         0x03ffU));
}

[[nodiscard]] std::vector<u16>
EncodeHalfRgba(
    const AtmosphereLut2D& lut)
{
    std::vector<u16> result;
    result.resize(
        lut.texels.size() *
        4U);

    for (std::size_t i = 0;
         i < lut.texels.size();
         ++i)
    {
        const auto value =
            lut.texels[i];

        result[i * 4U] =
            FloatToHalf(value.x);
        result[i * 4U + 1U] =
            FloatToHalf(value.y);
        result[i * 4U + 2U] =
            FloatToHalf(value.z);
        result[i * 4U + 3U] =
            FloatToHalf(value.w);
    }

    return result;
}
} // namespace

AtmosphereSample SampleAtmosphere(
    const AtmosphereParameters& p,
    const f64 radiusMeters)
{
    const f64 altitude =
        std::max(
            radiusMeters -
                p.bottomRadiusMeters,
            0.0);

    const f64 rayleighDensity =
        std::exp(
            -altitude /
            p.rayleighScaleHeightMeters);

    const f64 mieDensity =
        std::exp(
            -altitude /
            p.mieScaleHeightMeters);

    const f64 absorptionDensity =
        std::clamp(
            1.0 -
            std::abs(
                altitude -
                p.absorptionCenterHeightMeters) /
                p.absorptionHalfWidthMeters,
            0.0,
            1.0);

    const math::Double3 rayleigh =
        p.rayleighScatteringPerMeter *
        rayleighDensity;
    const math::Double3 mieScattering =
        p.mieScatteringPerMeter *
        mieDensity;
    const math::Double3 mieExtinction =
        p.mieExtinctionPerMeter *
        mieDensity;
    const math::Double3 absorption =
        p.absorptionExtinctionPerMeter *
        absorptionDensity;

    return {
        .altitudeMeters = altitude,
        .rayleighDensity =
            rayleighDensity,
        .mieDensity =
            mieDensity,
        .absorptionDensity =
            absorptionDensity,
        .scatteringPerMeter =
            rayleigh +
            mieScattering,
        .extinctionPerMeter =
            rayleigh +
            mieExtinction +
            absorption
    };
}

const math::Float4&
AtmosphereLut2D::At(
    const u32 x,
    const u32 y) const
{
    if (x >= width ||
        y >= height)
    {
        throw std::out_of_range(
            "Atmosphere LUT coordinate is out of range.");
    }

    return texels[
        static_cast<std::size_t>(y) *
            width +
        x];
}

u64 AtmosphereFingerprint(
    const AtmosphereParameters& p,
    const AtmosphereLutConfig& c)
{
    Validate(p, c);

    u64 seed =
        0x4d323141544d4f31ULL;

    for (const auto value : {
             p.bottomRadiusMeters,
             p.topRadiusMeters,
             p.rayleighScaleHeightMeters,
             p.mieScaleHeightMeters,
             p.mieAnisotropy,
             p.absorptionCenterHeightMeters,
             p.absorptionHalfWidthMeters})
    {
        seed =
            Combine(
                seed,
                HashDouble(value));
    }

    seed =
        Combine(
            seed,
            HashDouble3(
                p.rayleighScatteringPerMeter));
    seed =
        Combine(
            seed,
            HashDouble3(
                p.mieScatteringPerMeter));
    seed =
        Combine(
            seed,
            HashDouble3(
                p.mieExtinctionPerMeter));
    seed =
        Combine(
            seed,
            HashDouble3(
                p.absorptionExtinctionPerMeter));
    seed =
        Combine(
            seed,
            HashDouble3(
                p.groundAlbedo));

    for (const u32 value : {
             c.transmittanceWidth,
             c.transmittanceHeight,
             c.multiScatteringWidth,
             c.multiScatteringHeight,
             c.skyViewWidth,
             c.skyViewHeight,
             c.opticalDepthSteps,
             c.multiDirectionSamples,
             c.skyViewSteps})
    {
        seed =
            Combine(
                seed,
                value);
    }

    return seed;
}

AtmosphereStaticLuts BuildStaticLuts(
    const AtmosphereParameters& p,
    const AtmosphereLutConfig& c)
{
    const u64 fingerprint =
        AtmosphereFingerprint(
            p,
            c);

    AtmosphereStaticLuts result;
    result.fingerprint =
        fingerprint;

    auto& transmittance =
        result.transmittance;
    transmittance.width =
        c.transmittanceWidth;
    transmittance.height =
        c.transmittanceHeight;
    transmittance.fingerprint =
        Combine(
            fingerprint,
            0x5452414e534d4954ULL);
    transmittance.texels.resize(
        static_cast<std::size_t>(
            transmittance.width) *
        transmittance.height);

    for (u32 y = 0U;
         y < transmittance.height;
         ++y)
    {
        const f64 radius =
            RadiusFromUnit(
                p,
                (static_cast<f64>(y) +
                 0.5) /
                static_cast<f64>(
                    transmittance.height));

        const math::Double3 origin{
            0.0, 0.0, radius};

        for (u32 x = 0U;
             x < transmittance.width;
             ++x)
        {
            const f64 mu =
                -1.0 +
                2.0 *
                (static_cast<f64>(x) +
                 0.5) /
                static_cast<f64>(
                    transmittance.width);

            const f64 sinTheta =
                std::sqrt(
                    std::max(
                        1.0 -
                            mu * mu,
                        0.0));

            const math::Double3 direction{
                sinTheta, 0.0, mu};

            const auto value =
                TransmittanceAlong(
                    p,
                    origin,
                    direction,
                    c.opticalDepthSteps,
                    true);

            transmittance.texels[
                static_cast<
                    std::size_t>(y) *
                    transmittance.width +
                x] = {
                    static_cast<f32>(value.x),
                    static_cast<f32>(value.y),
                    static_cast<f32>(value.z),
                    1.0F
                };
        }
    }

    auto& multi =
        result.multiScattering;
    multi.width =
        c.multiScatteringWidth;
    multi.height =
        c.multiScatteringHeight;
    multi.fingerprint =
        Combine(
            fingerprint,
            0x4d554c5449534341ULL);
    multi.texels.resize(
        static_cast<std::size_t>(
            multi.width) *
        multi.height);

    constexpr f64 goldenAngle =
        2.39996322972865332223;

    for (u32 y = 0U;
         y < multi.height;
         ++y)
    {
        const f64 radius =
            RadiusFromUnit(
                p,
                (static_cast<f64>(y) +
                 0.5) /
                static_cast<f64>(
                    multi.height));

        const auto local =
            SampleAtmosphere(
                p,
                radius);

        const math::Double3 singleAlbedo =
            Clamp01(
                DivideSafe(
                    local.scatteringPerMeter,
                    local.extinctionPerMeter));

        const math::Double3 origin{
            0.0, 0.0, radius};

        math::Double3 averageEscape{};

        for (u32 sample = 0U;
             sample <
                c.multiDirectionSamples;
             ++sample)
        {
            const f64 z =
                1.0 -
                2.0 *
                (static_cast<f64>(
                     sample) +
                 0.5) /
                static_cast<f64>(
                    c.multiDirectionSamples);
            const f64 radial =
                std::sqrt(
                    std::max(
                        1.0 -
                            z * z,
                        0.0));
            const f64 azimuth =
                goldenAngle *
                static_cast<f64>(
                    sample);

            const math::Double3 direction{
                radial *
                    std::cos(azimuth),
                radial *
                    std::sin(azimuth),
                z};

            auto escape =
                TransmittanceAlong(
                    p,
                    origin,
                    direction,
                    c.opticalDepthSteps,
                    false);

            const auto boundary =
                BoundaryDistance(
                    p,
                    origin,
                    direction);

            if (boundary.groundHit)
            {
                escape =
                    Hadamard(
                        escape,
                        p.groundAlbedo);
            }

            averageEscape =
                averageEscape +
                escape;
        }

        averageEscape =
            averageEscape /
            static_cast<f64>(
                c.multiDirectionSamples);

        const math::Double3 recirculation =
            Hadamard(
                singleAlbedo,
                math::Double3{
                    1.0 -
                        averageEscape.x,
                    1.0 -
                        averageEscape.y,
                    1.0 -
                        averageEscape.z});

        const math::Double3 psi{
            recirculation.x <
                    0.999
                ? recirculation.x /
                      (1.0 -
                       recirculation.x)
                : 999.0,
            recirculation.y <
                    0.999
                ? recirculation.y /
                      (1.0 -
                       recirculation.y)
                : 999.0,
            recirculation.z <
                    0.999
                ? recirculation.z /
                      (1.0 -
                       recirculation.z)
                : 999.0
        };

        for (u32 x = 0U;
             x < multi.width;
             ++x)
        {
            const f64 sunMu =
                -1.0 +
                2.0 *
                (static_cast<f64>(x) +
                 0.5) /
                static_cast<f64>(
                    multi.width);

            const f64 sinSun =
                std::sqrt(
                    std::max(
                        1.0 -
                            sunMu * sunMu,
                        0.0));

            const auto direct =
                TransmittanceAlong(
                    p,
                    origin,
                    math::Double3{
                        sinSun,
                        0.0,
                        sunMu},
                    c.opticalDepthSteps,
                    true);

            const auto response =
                Hadamard(
                    direct,
                    psi);

            multi.texels[
                static_cast<
                    std::size_t>(y) *
                    multi.width +
                x] = {
                    static_cast<f32>(
                        response.x),
                    static_cast<f32>(
                        response.y),
                    static_cast<f32>(
                        response.z),
                    1.0F
                };
        }
    }

    return result;
}

u64 AtmosphereSkyFingerprint(
    const AtmosphereParameters& p,
    const u64 staticFingerprint,
    const SkyViewInput& input,
    const AtmosphereLutConfig& c)
{
    Validate(p, c);

    if (staticFingerprint !=
            AtmosphereFingerprint(
                p,
                c) ||
        !std::isfinite(
            input.observerRadiusMeters) ||
        input.observerRadiusMeters <=
            0.0 ||
        !std::isfinite(
            input.sunDirectionBody.x) ||
        !std::isfinite(
            input.sunDirectionBody.y) ||
        !std::isfinite(
            input.sunDirectionBody.z) ||
        math::LengthSquared(
            input.sunDirectionBody) <=
            1.0e-20)
    {
        throw std::invalid_argument(
            "Atmosphere sky fingerprint inputs are invalid.");
    }

    u64 fingerprint =
        Combine(
            staticFingerprint,
            HashDouble(
                input.observerRadiusMeters));
    fingerprint =
        Combine(
            fingerprint,
            HashDouble3(
                math::Normalize(
                    input.
                        sunDirectionBody)));
    fingerprint =
        Combine(
            fingerprint,
            HashDouble3(
                input.
                    incidentIrradianceWattsPerSquareMeter));

    return fingerprint;
}

AtmosphereSkyView BuildSkyView(
    const AtmosphereParameters& p,
    const AtmosphereStaticLuts& staticLuts,
    const SkyViewInput& input,
    const AtmosphereLutConfig& c)
{
    Validate(p, c);

    if (staticLuts.fingerprint !=
        AtmosphereFingerprint(p, c) ||
        !std::isfinite(
            input.observerRadiusMeters) ||
        input.observerRadiusMeters <=
            0.0 ||
        !std::isfinite(
            input.sunDirectionBody.x) ||
        !std::isfinite(
            input.sunDirectionBody.y) ||
        !std::isfinite(
            input.sunDirectionBody.z) ||
        math::LengthSquared(
            input.sunDirectionBody) <=
            1.0e-20)
    {
        throw std::invalid_argument(
            "Sky-view inputs do not match the atmosphere product.");
    }

    const math::Double3 sun =
        math::Normalize(
            input.sunDirectionBody);

    const f64 sunMu =
        sun.z;

    const u64 fingerprint =
        AtmosphereSkyFingerprint(
            p,
            staticLuts.fingerprint,
            input,
            c);

    AtmosphereSkyView result;
    result.observerRadiusMeters =
        input.observerRadiusMeters;
    result.sunDirectionBody =
        sun;
    result.incidentIrradianceWattsPerSquareMeter =
        input.incidentIrradianceWattsPerSquareMeter;
    result.fingerprint =
        fingerprint;

    auto& sky =
        result.skyView;
    sky.width =
        c.skyViewWidth;
    sky.height =
        c.skyViewHeight;
    sky.fingerprint =
        fingerprint;
    sky.texels.resize(
        static_cast<std::size_t>(
            sky.width) *
        sky.height);

    const math::Double3 origin{
        0.0,
        0.0,
        input.observerRadiusMeters};

    for (u32 y = 0U;
         y < sky.height;
         ++y)
    {
        const f64 viewTheta =
            std::numbers::pi_v<f64> *
            (static_cast<f64>(y) +
             0.5) /
            static_cast<f64>(
                sky.height);

        const f64 sinView =
            std::sin(viewTheta);
        const f64 cosView =
            std::cos(viewTheta);

        for (u32 x = 0U;
             x < sky.width;
             ++x)
        {
            const f64 relativeAzimuth =
                2.0 *
                std::numbers::pi_v<f64> *
                (static_cast<f64>(x) +
                 0.5) /
                static_cast<f64>(
                    sky.width);

            const math::Double3 view{
                sinView *
                    std::cos(
                        relativeAzimuth),
                sinView *
                    std::sin(
                        relativeAzimuth),
                cosView};

            f64 segmentStart = 0.0;
            f64 segmentEnd = 0.0;

            const f64 originRadius =
                math::Length(origin);

            if (originRadius <=
                p.topRadiusMeters)
            {
                const auto boundary =
                    BoundaryDistance(
                        p,
                        origin,
                        view);

                if (!std::isfinite(
                        boundary.distanceMeters))
                {
                    continue;
                }

                segmentEnd =
                    boundary.distanceMeters;
            }
            else
            {
                const auto topInterval =
                    RaySphereIntervalFor(
                        origin,
                        view,
                        p.topRadiusMeters);

                if (!topInterval.intersects ||
                    topInterval.farMeters <=
                        0.0)
                {
                    continue;
                }

                segmentStart =
                    std::max(
                        topInterval.nearMeters,
                        0.0);
                segmentEnd =
                    topInterval.farMeters;

                const auto groundInterval =
                    RaySphereIntervalFor(
                        origin,
                        view,
                        p.bottomRadiusMeters);

                if (groundInterval.intersects)
                {
                    const f64 groundEntry =
                        groundInterval.nearMeters >
                                segmentStart
                            ? groundInterval.nearMeters
                            : groundInterval.farMeters;

                    if (groundEntry >
                            segmentStart &&
                        groundEntry <
                            segmentEnd)
                    {
                        segmentEnd =
                            groundEntry;
                    }
                }
            }

            if (!(segmentEnd >
                  segmentStart))
            {
                continue;
            }

            const f64 step =
                (segmentEnd -
                 segmentStart) /
                static_cast<f64>(
                    c.skyViewSteps);

            math::Double3 opticalDepth{};
            math::Double3 radiance{};

            const f64 phaseCosine =
                std::clamp(
                    math::Dot(
                        view,
                        sun),
                    -1.0,
                    1.0);

            const f64 rayleighPhase =
                RayleighPhase(
                    phaseCosine);
            const f64 miePhase =
                MiePhase(
                    phaseCosine,
                    p.mieAnisotropy);

            for (u32 i = 0U;
                 i < c.skyViewSteps;
                 ++i)
            {
                const f64 t =
                    segmentStart +
                    (static_cast<f64>(i) +
                     0.5) *
                    step;

                const math::Double3 point =
                    origin +
                    view * t;

                const f64 radius =
                    math::Length(point);

                if (radius <
                        p.bottomRadiusMeters ||
                    radius >
                        p.topRadiusMeters)
                {
                    continue;
                }

                const auto local =
                    SampleAtmosphere(
                        p,
                        radius);

                const auto viewTransmittance =
                    ExpNeg(
                        opticalDepth);

                const math::Double3 up =
                    point / radius;

                const f64 localSunMu =
                    std::clamp(
                        math::Dot(
                            up,
                            sun),
                        -1.0,
                        1.0);

                const auto sunTransmittance =
                    LutRgb(
                        staticLuts.
                            transmittance,
                        localSunMu *
                                0.5 +
                            0.5,
                        UnitFromRadius(
                            p,
                            radius));

                const auto multiResponse =
                    LutRgb(
                        staticLuts.
                            multiScattering,
                        localSunMu *
                                0.5 +
                            0.5,
                        UnitFromRadius(
                            p,
                            radius));

                const math::Double3 rayleigh =
                    p.rayleighScatteringPerMeter *
                    local.rayleighDensity *
                    rayleighPhase;

                const math::Double3 mie =
                    p.mieScatteringPerMeter *
                    local.mieDensity *
                    miePhase;

                const math::Double3 singleSource =
                    Hadamard(
                        input.
                            incidentIrradianceWattsPerSquareMeter,
                        Hadamard(
                            sunTransmittance,
                            rayleigh + mie));

                const math::Double3 multipleSource =
                    Hadamard(
                        input.
                            incidentIrradianceWattsPerSquareMeter,
                        Hadamard(
                            multiResponse,
                            local.scatteringPerMeter)) /
                    (4.0 *
                     std::numbers::pi_v<f64>);

                radiance =
                    radiance +
                    Hadamard(
                        viewTransmittance,
                        singleSource +
                            multipleSource) *
                        step;

                opticalDepth =
                    opticalDepth +
                    local.extinctionPerMeter *
                        step;
            }

            sky.texels[
                static_cast<
                    std::size_t>(y) *
                    sky.width +
                x] = {
                    static_cast<f32>(
                        std::max(
                            radiance.x,
                            0.0)),
                    static_cast<f32>(
                        std::max(
                            radiance.y,
                            0.0)),
                    static_cast<f32>(
                        std::max(
                            radiance.z,
                            0.0)),
                    1.0F
                };
        }
    }

    return result;
}

GpuAtmosphereLuts::UploadTexture
GpuAtmosphereLuts::CreateTexture(
    rhi::Device& device,
    const AtmosphereLut2D& lut)
{
    if (lut.width == 0U ||
        lut.height == 0U ||
        lut.texels.size() !=
            static_cast<std::size_t>(
                lut.width) *
                lut.height)
    {
        throw std::invalid_argument(
            "Atmosphere LUT GPU upload requires a valid LUT.");
    }

    const auto encoded =
        EncodeHalfRgba(lut);

    UploadTexture result;

    result.staging =
        device.CreateBuffer({
            .sizeBytes =
                static_cast<u64>(
                    encoded.size() *
                    sizeof(u16)),
            .usage =
                rhi::BufferUsage::Generic,
            .memory =
                rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::CopySource
        });

    result.texture =
        device.CreateTexture({
            .width = lut.width,
            .height = lut.height,
            .format =
                rhi::TextureFormat::
                    RGBA16_Float,
            .initialState =
                rhi::ResourceState::
                    CopyDestination
        });

    if (!result.staging ||
        !result.texture)
    {
        throw std::runtime_error(
            "Failed to allocate atmosphere LUT GPU resources.");
    }

    std::memcpy(
        result.staging->Map(),
        encoded.data(),
        encoded.size() *
            sizeof(u16));
    result.staging->Unmap();

    return result;
}

void GpuAtmosphereLuts::EnsureTextureUploaded(
    rhi::CommandList& commands,
    UploadTexture& product)
{
    if (product.uploaded)
    {
        return;
    }

    commands.CopyBufferToTexture(
        *product.staging,
        0,
        *product.texture);

    commands.Transition(
        *product.texture,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::ShaderResource);

    product.uploaded = true;
}

GpuAtmosphereLuts::GpuAtmosphereLuts(
    rhi::Device& device,
    const AtmosphereStaticLuts& staticLuts,
    const AtmosphereSkyView& skyView)
    : transmittance_(
          CreateTexture(
              device,
              staticLuts.transmittance)),
      multiScattering_(
          CreateTexture(
              device,
              staticLuts.multiScattering)),
      skyView_(
          CreateTexture(
              device,
              skyView.skyView)),
      staticFingerprint_(
          staticLuts.fingerprint),
      skyFingerprint_(
          skyView.fingerprint)
{
}

void GpuAtmosphereLuts::EnsureUploaded(
    rhi::CommandList& commands)
{
    EnsureTextureUploaded(
        commands,
        transmittance_);
    EnsureTextureUploaded(
        commands,
        multiScattering_);
    EnsureTextureUploaded(
        commands,
        skyView_);
}

rhi::Texture&
GpuAtmosphereLuts::Transmittance() noexcept
{
    return *transmittance_.texture;
}

rhi::Texture&
GpuAtmosphereLuts::MultiScattering() noexcept
{
    return *multiScattering_.texture;
}

rhi::Texture&
GpuAtmosphereLuts::SkyView() noexcept
{
    return *skyView_.texture;
}

u64 GpuAtmosphereLuts::StaticFingerprint() const noexcept
{
    return staticFingerprint_;
}

u64 GpuAtmosphereLuts::SkyFingerprint() const noexcept
{
    return skyFingerprint_;
}
} // namespace orbit::celestial_atmosphere
