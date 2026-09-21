#include <orbit/celestial_ocean/OceanOptics.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace orbit::celestial_ocean
{
namespace
{
void Validate(
    const OceanOpticalParameters& p)
{
    if (!std::isfinite(p.refractiveIndex) ||
        p.refractiveIndex < 1.0 ||
        !std::isfinite(p.orbitalRoughness) ||
        p.orbitalRoughness <= 0.0 ||
        p.orbitalRoughness > 1.0 ||
        !std::isfinite(p.glintStrength) ||
        p.glintStrength < 0.0 ||
        !std::isfinite(p.deepColorDepthMeters) ||
        p.deepColorDepthMeters <= 0.0 ||
        p.absorptionPerMeter.x < 0.0 ||
        p.absorptionPerMeter.y < 0.0 ||
        p.absorptionPerMeter.z < 0.0 ||
        p.deepWaterColor.x < 0.0 ||
        p.deepWaterColor.y < 0.0 ||
        p.deepWaterColor.z < 0.0)
    {
        throw std::invalid_argument(
            "Ocean optical parameters are invalid.");
    }
}

[[nodiscard]] f64 SmithG1(
    const f64 nDotX,
    const f64 alpha) noexcept
{
    if (nDotX <= 0.0)
        return 0.0;

    const f64 a2 = alpha * alpha;
    const f64 n2 = nDotX * nDotX;
    return
        2.0 * nDotX /
        (nDotX +
         std::sqrt(
             a2 +
             (1.0 - a2) * n2));
}
} // namespace

f64 DielectricNormalReflectance(
    const f64 outside,
    const f64 inside)
{
    if (!std::isfinite(outside) ||
        !std::isfinite(inside) ||
        outside <= 0.0 ||
        inside <= 0.0)
    {
        throw std::invalid_argument(
            "Refractive indices must be finite and positive.");
    }

    const f64 ratio =
        (outside - inside) /
        (outside + inside);

    return ratio * ratio;
}

f64 SchlickFresnel(
    const f64 cosineTheta,
    const f64 f0)
{
    const f64 c =
        std::clamp(cosineTheta, 0.0, 1.0);
    const f64 oneMinus =
        1.0 - c;
    return
        f0 +
        (1.0 - f0) *
        oneMinus * oneMinus *
        oneMinus * oneMinus *
        oneMinus;
}

f64 GgxSpecularBrdf(
    const f64 nDotL,
    const f64 nDotV,
    const f64 nDotH,
    const f64 vDotH,
    const f64 roughness,
    const f64 f0)
{
    const f64 nl =
        std::clamp(nDotL, 0.0, 1.0);
    const f64 nv =
        std::clamp(nDotV, 0.0, 1.0);
    const f64 nh =
        std::clamp(nDotH, 0.0, 1.0);

    if (nl <= 0.0 || nv <= 0.0)
        return 0.0;

    const f64 alpha =
        std::max(
            roughness * roughness,
            1.0e-4);
    const f64 a2 =
        alpha * alpha;

    const f64 denom =
        nh * nh * (a2 - 1.0) + 1.0;

    const f64 d =
        a2 /
        (std::numbers::pi_v<f64> *
         denom * denom);

    const f64 g =
        SmithG1(nl, alpha) *
        SmithG1(nv, alpha);

    const f64 f =
        SchlickFresnel(
            vDotH,
            f0);

    return
        d * g * f /
        std::max(
            4.0 * nl * nv,
            1.0e-8);
}

math::Double3 WaterColumnTransmittance(
    const math::Double3& absorption,
    const f64 depth)
{
    if (depth < 0.0 ||
        absorption.x < 0.0 ||
        absorption.y < 0.0 ||
        absorption.z < 0.0)
    {
        throw std::invalid_argument(
            "Water absorption/depth is invalid.");
    }

    return {
        std::exp(-absorption.x * depth),
        std::exp(-absorption.y * depth),
        std::exp(-absorption.z * depth)
    };
}

math::Double3 DeepWaterColor(
    const OceanOpticalParameters& p,
    const f64 depth)
{
    Validate(p);

    const f64 normalizedDepth =
        std::clamp(
            depth /
                p.deepColorDepthMeters,
            0.0,
            1.0);

    const auto transmission =
        WaterColumnTransmittance(
            p.absorptionPerMeter,
            std::max(depth, 0.0));

    return {
        p.deepWaterColor.x *
            (1.0 - transmission.x) *
            normalizedDepth,
        p.deepWaterColor.y *
            (1.0 - transmission.y) *
            normalizedDepth,
        p.deepWaterColor.z *
            (1.0 - transmission.z) *
            normalizedDepth
    };
}

void ApplyOrbitalOceanAppearance(
    celestial_appearance::PlanetaryAppearanceProduct& appearance,
    const OceanOpticalParameters& p)
{
    Validate(p);

    for (auto& texel : appearance.texels)
    {
        const f32 ocean =
            std::clamp(
                texel.oceanMask,
                0.0F,
                1.0F);

        if (ocean <= 0.0F)
            continue;

        const auto deep =
            DeepWaterColor(
                p,
                std::max(
                    static_cast<f64>(
                        texel.waterDepthMeters),
                    0.0));

        const f32 depthBlend =
            static_cast<f32>(
                std::clamp(
                    static_cast<f64>(
                        texel.waterDepthMeters) /
                        p.deepColorDepthMeters,
                    0.0,
                    1.0));

        const math::Float3 deepColor{
            static_cast<f32>(deep.x),
            static_cast<f32>(deep.y),
            static_cast<f32>(deep.z)
        };

        const f32 blend =
            ocean * depthBlend *
            (1.0F - texel.iceMask);

        texel.albedoLinear =
            texel.albedoLinear *
                (1.0F - blend) +
            deepColor * blend;

        texel.roughness =
            std::clamp(
                texel.roughness *
                    (1.0F - ocean) +
                static_cast<f32>(
                    p.orbitalRoughness) *
                    ocean,
                0.01F,
                1.0F);
    }

    appearance.fingerprint =
        terrain::StableCombine64(
            appearance.fingerprint,
            OceanOpticalFingerprint(p));
}

u64 OceanOpticalFingerprint(
    const OceanOpticalParameters& p)
{
    Validate(p);

    u64 value =
        0x4d32344f4345414eULL;

    const auto add =
        [&value](const f64 v)
        {
            value =
                terrain::StableCombine64(
                    value,
                    std::bit_cast<u64>(v));
        };

    add(p.refractiveIndex);
    add(p.orbitalRoughness);
    add(p.absorptionPerMeter.x);
    add(p.absorptionPerMeter.y);
    add(p.absorptionPerMeter.z);
    add(p.deepWaterColor.x);
    add(p.deepWaterColor.y);
    add(p.deepWaterColor.z);
    add(p.glintStrength);
    add(p.deepColorDepthMeters);

    return value;
}
} // namespace orbit::celestial_ocean
