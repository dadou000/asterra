#include <orbit/celestial_far_render/FarBodyRenderer.hpp>

#include <orbit/celestial_lighting/CelestialLighting.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace orbit::celestial_far_render
{
namespace
{
[[nodiscard]] math::Double3 DiscDirection(
    const f64 x,
    const f64 y)
{
    const f64 r2 =
        x * x + y * y;

    if (r2 > 1.0)
    {
        return {};
    }

    return math::Normalize(
        math::Double3{
            x,
            y,
            std::sqrt(
                std::max(
                    1.0 - r2,
                    0.0))
        });
}

[[nodiscard]] const celestial_appearance::AppearanceTexel&
SampleAppearance(
    const celestial_appearance::PlanetaryAppearanceProduct& appearance,
    const math::Double3 d)
{
    const f64 ax = std::abs(d.x);
    const f64 ay = std::abs(d.y);
    const f64 az = std::abs(d.z);

    u32 face = 0U;
    f64 u = 0.0;
    f64 v = 0.0;

    if (ax >= ay && ax >= az)
    {
        if (d.x >= 0.0)
        {
            face = 0U;
            u = -d.z / ax;
            v = d.y / ax;
        }
        else
        {
            face = 1U;
            u = d.z / ax;
            v = d.y / ax;
        }
    }
    else if (ay >= ax && ay >= az)
    {
        if (d.y >= 0.0)
        {
            face = 2U;
            u = d.x / ay;
            v = -d.z / ay;
        }
        else
        {
            face = 3U;
            u = d.x / ay;
            v = d.z / ay;
        }
    }
    else
    {
        if (d.z >= 0.0)
        {
            face = 4U;
            u = d.x / az;
            v = d.y / az;
        }
        else
        {
            face = 5U;
            u = -d.x / az;
            v = d.y / az;
        }
    }

    const f64 fx =
        (u * 0.5 + 0.5) *
        static_cast<f64>(
            appearance.faceResolution - 1U);
    const f64 fy =
        (v * 0.5 + 0.5) *
        static_cast<f64>(
            appearance.faceResolution - 1U);

    const u32 x =
        std::min(
            static_cast<u32>(
                std::llround(fx)),
            appearance.faceResolution - 1U);
    const u32 y =
        std::min(
            static_cast<u32>(
                std::llround(fy)),
            appearance.faceResolution - 1U);

    return appearance.At(
        face,
        x,
        y);
}

[[nodiscard]] u16 FloatToHalfBits(
    const f32 value) noexcept
{
    const u32 bits = std::bit_cast<u32>(value);
    const u32 sign = (bits >> 16U) & 0x8000U;
    const u32 exponent = (bits >> 23U) & 0xFFU;
    u32 mantissa = bits & 0x7FFFFFU;

    if (exponent == 0xFFU)
    {
        return static_cast<u16>(
            sign |
            (mantissa == 0U ? 0x7C00U : 0x7E00U));
    }

    const i32 halfExponent =
        static_cast<i32>(exponent) - 127 + 15;

    if (halfExponent >= 31)
    {
        return static_cast<u16>(sign | 0x7C00U);
    }

    if (halfExponent <= 0)
    {
        if (halfExponent < -10)
        {
            return static_cast<u16>(sign);
        }

        mantissa |= 0x800000U;
        const u32 shift =
            static_cast<u32>(14 - halfExponent);
        u32 halfMantissa = mantissa >> shift;
        if ((mantissa >> (shift - 1U)) & 1U)
        {
            ++halfMantissa;
        }

        return static_cast<u16>(
            sign |
            (halfMantissa & 0x03FFU));
    }

    u32 halfMantissa = mantissa >> 13U;
    if (mantissa & 0x00001000U)
    {
        ++halfMantissa;
        if (halfMantissa == 0x0400U)
        {
            halfMantissa = 0U;
            const u32 roundedExponent =
                static_cast<u32>(halfExponent + 1);
            if (roundedExponent >= 31U)
            {
                return static_cast<u16>(sign | 0x7C00U);
            }

            return static_cast<u16>(
                sign |
                (roundedExponent << 10U));
        }
    }

    return static_cast<u16>(
        sign |
        (static_cast<u32>(halfExponent) << 10U) |
        (halfMantissa & 0x03FFU));
}

[[nodiscard]] u64 DiscFingerprint(
    const celestial_appearance::PlanetaryAppearanceProduct& appearance,
    const CachedDiscConfig& config)
{
    u64 value =
        0x4D31384449534331ULL;
    value =
        terrain::StableCombine64(
            value,
            appearance.fingerprint);
    value =
        terrain::StableCombine64(
            value,
            config.resolution);
    return value;
}

[[nodiscard]] universe::EllipsoidShape AsEllipsoid(
    const universe::BodyShape& shape)
{
    return std::visit(
        [](const auto& value)
        {
            using Shape =
                std::decay_t<
                    decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Shape,
                    universe::SphereShape>)
            {
                return universe::EllipsoidShape{
                    .radiiMeters = {
                        value.radiusMeters,
                        value.radiusMeters,
                        value.radiusMeters
                    }
                };
            }
            else
            {
                return value;
            }
        },
        shape);
}

constexpr const char* kQuadVs = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    const float2 p[6] =
    {
        float2(-1,-1),
        float2(-1, 1),
        float2( 1,-1),
        float2( 1,-1),
        float2(-1, 1),
        float2( 1, 1)
    };

    VSOutput o;
    o.position = float4(p[vertexId], 0, 1);
    o.uv = p[vertexId];
    return o;
}
)";

constexpr const char* kAnalyticPs = R"(
struct Constants
{
    float4 radiiAndAspect;
    float4 cameraAndTanHalfFov;
    float4 forward;
    float4 up;
    float4 albedoAndRoughness;
    float4 material;
    float4 emissionAndOpacity;
    float4 proxy;
    float4 lighting;
    float4 ocean;
};
[[vk::push_constant]] Constants g;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float OceanSpecular(
    float3 n,
    float3 l,
    float3 v,
    float roughness,
    float eta,
    float strength)
{
    const float ndl = saturate(dot(n, l));
    const float ndv = saturate(dot(n, v));
    if (ndl <= 0.0 || ndv <= 0.0)
        return 0.0;

    const float3 h = normalize(l + v);
    const float ndh = saturate(dot(n, h));
    const float vdh = saturate(dot(v, h));
    const float f0 =
        pow((max(eta, 1.0) - 1.0) /
            (max(eta, 1.0) + 1.0), 2.0);

    const float r = clamp(roughness, 0.01, 1.0);
    const float alpha = max(r * r, 0.0001);
    const float a2 = alpha * alpha;
    const float denom = ndh * ndh * (a2 - 1.0) + 1.0;
    const float D =
        a2 / max(3.14159265 * denom * denom, 1e-6);
    const float k =
        (r + 1.0) * (r + 1.0) / 8.0;
    const float Gl =
        ndl / max(ndl * (1.0 - k) + k, 1e-5);
    const float Gv =
        ndv / max(ndv * (1.0 - k) + k, 1e-5);
    const float F =
        f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);

    return
        max(strength, 0.0) *
        D * Gl * Gv * F /
        max(4.0 * ndl * ndv, 1e-5);
}

uint StellarHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float StellarNoise(float3 p, float scale, uint seed)
{
    int3 q = (int3)floor(p * max(scale, 1.0) * 4096.0);
    uint h = StellarHash(seed ^ asuint(q.x));
    h = StellarHash(h ^ asuint(q.y));
    h = StellarHash(h ^ asuint(q.z));
    return (float)(h & 0x00ffffffu) / 16777215.0;
}

float StellarFractal(float3 p, float scale, uint seed)
{
    float sum = 0.0;
    float weight = 0.0;
    float amplitude = 1.0;
    float frequency = scale;
    [unroll]
    for (uint octave = 0u; octave < 4u; ++octave)
    {
        sum += StellarNoise(p, frequency, seed + octave * 0x9e3779b9u) * amplitude;
        weight += amplitude;
        amplitude *= 0.5;
        frequency *= 2.07;
    }
    return sum / max(weight, 1e-6);
}

float StellarSurfaceModulation(float3 n)
{
    const float granulationStrength = saturate(g.material.x);
    const float activityLevel = saturate(g.material.y);
    const uint seed = asuint(g.material.w);
    const float granulationScale = max(g.emissionAndOpacity.x, 1.0);

    const float granular =
        1.0 +
        (StellarFractal(n, granulationScale, seed ^ 0x4752414eu) - 0.5) *
        2.0 * granulationStrength;

    const float activity =
        StellarFractal(n, 3.7, seed ^ 0x41435449u);
    const float threshold =
        1.0 - 0.10 * activityLevel;

    float activityModulation = 1.0;
    if (activity > threshold)
    {
        const float spot =
            saturate((activity - threshold) / max(1.0 - threshold, 1e-6));
        activityModulation =
            1.0 - 0.55 * spot;
    }
    else
    {
        const float facula =
            max(0.0, activity - (threshold - 0.08));
        activityModulation =
            1.0 + facula * 0.35 * activityLevel;
    }

    return max(granular * activityModulation, 0.0);
}

float3 GiantSurfaceColor(float3 n)
{
    const float frequency = max(g.material.x, 1.0);
    const float bandStrength = saturate(g.material.y);
    const uint seed = asuint(g.material.w);
    const float zonalShear = max(g.albedoAndRoughness.w, 0.0);
    const float stormStrength = saturate(g.proxy.z);
    const float stormScale = max(g.proxy.w, 0.25);
    const float polarStrength = saturate(g.ocean.w);
    const float depthContrast = saturate(g.forward.w);
    const float turbulenceStrength = saturate(g.up.w);

    const float latitude = asin(clamp(n.y, -1.0, 1.0));
    const float longitude = atan2(n.z, n.x);

    const float broad =
        StellarFractal(n, 3.2, seed ^ 0x42524f41u) - 0.5;
    const float fine =
        StellarFractal(
            n,
            max(frequency * 1.7, 4.0),
            seed ^ 0x54555242u) - 0.5;

    const float shear =
        zonalShear *
        (0.55 * broad + 0.45 * fine) *
        cos(latitude);

    const float wave =
        sin(
            latitude * frequency * 3.14159265 +
            longitude * 0.18 +
            shear * 4.0);

    const float narrow =
        sin(
            latitude * frequency * 2.07 * 3.14159265 -
            longitude * 0.11 +
            fine * 1.7);

    float band =
        saturate(
            0.5 +
            (0.5 + 0.5 * (0.72 * wave + 0.28 * narrow) - 0.5) *
            (0.35 + 1.3 * bandStrength));

    float3 color =
        lerp(
            g.albedoAndRoughness.xyz,
            g.emissionAndOpacity.xyz,
            band);

    const float depth =
        1.0 +
        depthContrast *
        (0.65 * broad + 0.35 * fine) *
        0.55;
    color *= max(depth, 0.2);

    const float stormNoise =
        StellarFractal(
            n,
            stormScale,
            seed ^ 0x53544f52u);

    const float stormMask =
        smoothstep(
            0.82 - 0.18 * stormStrength,
            0.985,
            stormNoise);

    color *=
        1.0 +
        stormMask *
        stormStrength *
        (0.22 + 0.18 * sin(longitude * 5.0));

    color *=
        1.0 +
        fine *
        turbulenceStrength *
        0.20;

    const float polar =
        pow(abs(n.y), 2.7) *
        polarStrength;

    color =
        lerp(
            color,
            g.ocean.xyz,
            saturate(polar));

    return max(color, 0.0);
}

float SmallBodyHash01(uint value)
{
    return (float)(StellarHash(value) & 0x00ffffffu) / 16777215.0;
}

float2 SmallBodyUnpack01(uint packed)
{
    return float2(
        (float)(packed & 0xffffu) / 65535.0,
        (float)((packed >> 16u) & 0xffffu) / 65535.0);
}

float SmallBodyCraterRelief(float3 d)
{
    const float density = saturate(g.ocean.z);
    const float2 craterShape =
        SmallBodyUnpack01(asuint(g.forward.w));
    const float craterDepth = craterShape.x * 0.5;
    const float craterRimStrength = craterShape.y * 0.5;
    const uint seed = asuint(g.material.w);
    const uint count =
        2u + (uint)round(density * 10.0);

    float relief = 0.0;

    [unroll]
    for (uint i = 0u; i < 12u; ++i)
    {
        if (i >= count)
            continue;

        const float z =
            SmallBodyHash01(seed + i * 41u + 7u) * 2.0 - 1.0;
        const float azimuth =
            SmallBodyHash01(seed + i * 73u + 19u) *
            6.28318530718;
        const float radial =
            sqrt(max(1.0 - z * z, 0.0));
        const float3 center =
            float3(
                radial * cos(azimuth),
                z,
                radial * sin(azimuth));

        const float angle =
            acos(clamp(dot(d, center), -1.0, 1.0));
        const float radius =
            0.035 +
            0.11 *
            SmallBodyHash01(seed + i * 137u + 29u);
        const float q =
            angle / max(radius, 1e-5);

        const float bowl =
            q < 1.0
                ? -(1.0 - q * q)
                : 0.0;

        const float rim =
            smoothstep(1.35, 1.0, q) -
            smoothstep(1.0, 0.86, q);

        relief +=
            bowl * craterDepth +
            rim * craterRimStrength;
    }

    return relief;
}

float SmallBodyRadiusScale(float3 direction)
{
    const float3 d = normalize(direction);
    const float3 axes =
        max(
            float3(
                g.material.x,
                g.material.y,
                g.proxy.z),
            float3(0.05, 0.05, 0.05));

    const float ellipsoid =
        rsqrt(
            dot(
                d * d,
                1.0 / (axes * axes)));

    const uint seed = asuint(g.material.w);
    const float irregularity = saturate(g.ocean.x);
    const float lobeStrength = saturate(g.ocean.y);

    const float broad =
        (StellarFractal(
            d,
            2.1,
            seed ^ 0x53484142u) - 0.5) * 2.0;
    const float medium =
        (StellarFractal(
            d,
            5.7,
            seed ^ 0x5348414du) - 0.5) * 2.0;

    const float lobe =
        sin(
            atan2(d.z, d.x) * 2.0 +
            d.y * 2.7 +
            SmallBodyHash01(seed) * 6.0) *
        lobeStrength;

    return max(
        0.18,
        ellipsoid *
        (1.0 +
         irregularity *
             (0.68 * broad + 0.32 * medium) +
         lobe +
         SmallBodyCraterRelief(d)));
}

float SmallBodyField(float3 normalizedPosition)
{
    const float radial =
        length(normalizedPosition);
    if (radial <= 1e-6)
        return -1.0;

    return radial -
        SmallBodyRadiusScale(
            normalizedPosition / radial);
}

float3 SmallBodyNormal(
    float3 q,
    float3 radii)
{
    const float e = 0.0025;
    const float3 dx = float3(e, 0.0, 0.0);
    const float3 dy = float3(0.0, e, 0.0);
    const float3 dz = float3(0.0, 0.0, e);

    const float3 gradient =
        float3(
            SmallBodyField(q + dx) -
                SmallBodyField(q - dx),
            SmallBodyField(q + dy) -
                SmallBodyField(q - dy),
            SmallBodyField(q + dz) -
                SmallBodyField(q - dz));

    return normalize(
        gradient /
        max(radii, float3(0.001, 0.001, 0.001)));
}

bool SmallBodyIntersect(
    float3 ro,
    float3 rd,
    float3 radii,
    out float tHit,
    out float3 normal)
{
    const float3 axes =
        max(
            float3(
                g.material.x,
                g.material.y,
                g.proxy.z),
            float3(0.05, 0.05, 0.05));
    const float bound =
        max(axes.x, max(axes.y, axes.z)) *
        (1.0 +
         saturate(g.ocean.x) +
         saturate(g.ocean.y) +
         0.55);

    const float a = dot(rd, rd);
    const float b = 2.0 * dot(ro, rd);
    const float cc =
        dot(ro, ro) -
        bound * bound;
    const float discriminant =
        b * b - 4.0 * a * cc;

    if (discriminant < 0.0)
        return false;

    const float root =
        sqrt(discriminant);
    const float enter =
        max(
            (-b - root) /
                max(2.0 * a, 1e-6),
            0.0);
    const float exit =
        (-b + root) /
        max(2.0 * a, 1e-6);

    if (exit <= enter)
        return false;

    float previousT = enter;
    float previousField =
        SmallBodyField(
            ro + rd * previousT);

    [loop]
    for (uint step = 1u; step <= 28u; ++step)
    {
        const float currentT =
            lerp(
                enter,
                exit,
                (float)step / 28.0);
        const float currentField =
            SmallBodyField(
                ro + rd * currentT);

        if (currentField <= 0.0 &&
            previousField > 0.0)
        {
            float low = previousT;
            float high = currentT;

            [unroll]
            for (uint refine = 0u; refine < 8u; ++refine)
            {
                const float mid =
                    0.5 * (low + high);
                const float midField =
                    SmallBodyField(
                        ro + rd * mid);

                if (midField > 0.0)
                    low = mid;
                else
                    high = mid;
            }

            tHit = high;
            const float3 q =
                ro + rd * tHit;
            normal =
                SmallBodyNormal(
                    q,
                    radii);
            return true;
        }

        previousT = currentT;
        previousField = currentField;
    }

    return false;
}

float3 SmallBodySurfaceColor(float3 n)
{
    const uint seed = asuint(g.material.w);
    const float2 materialVariation =
        SmallBodyUnpack01(asuint(g.up.w));
    const float singleScatteringAlbedo =
        materialVariation.x;
    const float colorVariation =
        materialVariation.y;

    const float variation =
        (StellarFractal(
            n,
            13.0,
            seed ^ 0x434f4c4fu) - 0.5) * 2.0;

    const float craterFreshness =
        saturate(
            -SmallBodyCraterRelief(n) *
            4.0);

    const float freshness =
        saturate(
            craterFreshness * 0.75 +
            max(variation, 0.0) * 0.18);

    float3 color =
        lerp(
            g.albedoAndRoughness.xyz,
            g.emissionAndOpacity.xyz,
            freshness);

    color *=
        max(
            0.2,
            1.0 +
            variation *
            colorVariation);

    // Keep the authored particulate single-scattering albedo visible in the
    // live representation without turning it into a second albedo authority.
    color *=
        lerp(
            0.72,
            1.18,
            saturate(singleScatteringAlbedo));

    return max(color, 0.0);
}

float SmallBodyPhotometry(
    float3 n,
    float3 l,
    float3 v)
{
    const float mu0 =
        max(dot(n, l), 0.0);
    const float mu =
        max(dot(n, v), 0.0);

    if (mu0 <= 0.0 || mu <= 0.0)
        return 0.0;

    const float phaseAngle =
        acos(clamp(dot(l, v), -1.0, 1.0));
    const float oppositionStrength =
        max(g.proxy.w, 0.0);
    const float oppositionWidth =
        max(g.ocean.w, 1e-5);
    const float macroRoughness =
        max(g.albedoAndRoughness.w, 0.0);
    const float singleScatteringAlbedo =
        SmallBodyUnpack01(asuint(g.up.w)).x;

    const float opposition =
        1.0 +
        oppositionStrength /
        (1.0 +
         tan(phaseAngle * 0.5) /
         oppositionWidth);

    const float lommelSeeliger =
        mu0 /
        max(mu0 + mu, 1e-5);

    const float roughnessShadowing =
        exp(
            -macroRoughness *
             macroRoughness *
             (1.0 - mu0) *
             (1.0 - mu));

    const float multipleScatter =
        singleScatteringAlbedo *
        mu0 *
        0.18;

    return max(
        0.0,
        (lommelSeeliger *
             opposition +
         multipleScatter) *
        roughnessShadowing);
}

float4 main(VSOutput input) : SV_Target0
{
    const uint mode = (uint)round(g.proxy.x);
    const float opacity = saturate(g.emissionAndOpacity.w);
    const float2 p = input.uv;

    if (mode == 1u)
    {
        const float radiusNdc =
            max(g.proxy.y, 0.00025);
        const float2 q =
            p / radiusNdc;
        const float r2 =
            dot(q, q);
        const float r =
            sqrt(max(r2, 0.0));
        const float stellar =
            abs(g.material.z - 1.0) < 0.25
                ? 1.0
                : 0.0;
        const float giant =
            abs(g.material.z - 2.0) < 0.25
                ? 1.0
                : 0.0;
        const float radiometricIntensity =
            max(g.proxy.w, 0.0);

        if (stellar > 0.5)
        {
            const float chromosphereStrength =
                max(g.emissionAndOpacity.y, 0.0);
            const float chromosphereExtent =
                max(g.emissionAndOpacity.z, 0.0);
            const float coronaStrength =
                max(g.lighting.x, 0.0);
            const float coronaExtent =
                max(g.lighting.y, 0.0);
            const float outer =
                1.0 +
                max(chromosphereExtent, coronaExtent);

            if (r > outer)
                discard;

            if (r <= 1.0)
            {
                const float z =
                    sqrt(max(1.0 - r2, 0.0));
                const float3 n =
                    normalize(float3(q.x, -q.y, z));
                const float limbCoefficient =
                    saturate(g.albedoAndRoughness.w);
                const float limb =
                    1.0 -
                    limbCoefficient *
                    (1.0 - z);
                const float modulation =
                    StellarSurfaceModulation(n);

                return float4(
                    g.albedoAndRoughness.xyz *
                        radiometricIntensity *
                        limb *
                        modulation,
                    opacity);
            }

            float chromosphere = 0.0;
            if (chromosphereExtent > 0.0)
            {
                const float x =
                    (r - 1.0) /
                    max(chromosphereExtent, 1e-6);
                if (x <= 1.0)
                    chromosphere =
                        chromosphereStrength *
                        exp(-4.0 * x);
            }

            float corona = 0.0;
            if (coronaExtent > 0.0)
            {
                const float x =
                    (r - 1.0) /
                    max(coronaExtent, 1e-6);
                if (x <= 1.0)
                    corona =
                        coronaStrength /
                        pow(1.0 + 7.0 * x, 2.25);
            }

            const float3 chromaColor =
                float3(1.0, 0.20, 0.08);
            const float3 coronaColor =
                lerp(
                    g.albedoAndRoughness.xyz,
                    float3(0.82, 0.90, 1.0),
                    0.72);

            const float3 color =
                radiometricIntensity *
                (chromaColor * chromosphere +
                 coronaColor * corona);

            const float haloAlpha =
                saturate(
                    chromosphere * 5.0 +
                    corona * 10.0);

            return float4(
                color,
                opacity * haloAlpha);
        }

        const float smallBody =
            g.material.z > 2.5
                ? 1.0
                : 0.0;

        float3 n;

        if (smallBody > 0.5)
        {
            const float3 edgeDirection =
                normalize(
                    float3(
                        q.x,
                        -q.y,
                        sqrt(
                            max(
                                1.0 -
                                min(r2, 1.0),
                                0.0))));

            const float radiusScale =
                SmallBodyRadiusScale(
                    edgeDirection);

            if (r > radiusScale)
                discard;

            const float2 sq =
                q /
                max(radiusScale, 0.05);
            const float sr2 =
                dot(sq, sq);

            if (sr2 > 1.0)
                discard;

            n =
                normalize(
                    float3(
                        sq.x,
                        -sq.y,
                        sqrt(
                            max(
                                1.0 - sr2,
                                0.0))));
        }
        else
        {
            if (r2 > 1.0)
                discard;

            const float z =
                sqrt(
                    max(
                        1.0 - r2,
                        0.0));

            n =
                normalize(
                    float3(
                        q.x,
                        -q.y,
                        z));
        }

        const float3 l =
            normalize(g.lighting.xyz);

        const float ndl =
            saturate(
                dot(n, l));

        const float roughness =
            giant > 0.5
                ? 0.90
                : saturate(
                      g.albedoAndRoughness.w);
        const float ocean =
            giant > 0.5
                ? 0.0
                : saturate(
                      g.material.x);
        const float ice =
            giant > 0.5
                ? 0.0
                : saturate(
                      g.material.y);

        const float cloudTransmission =
            giant > 0.5
                ? 1.0
                : saturate(g.material.w);
        const float3 v =
            normalize(float3(-q.x, q.y, z));
        const float glint =
            ocean *
            (1.0 - ice) *
            saturate(g.ocean.w) *
            OceanSpecular(
                n,
                l,
                v,
                g.ocean.y,
                g.ocean.x,
                g.ocean.z);

        const float3 surfaceColor =
            giant > 0.5
                ? GiantSurfaceColor(n)
                : smallBody > 0.5
                    ? SmallBodySurfaceColor(n)
                    : g.albedoAndRoughness.xyz;

        const float particulate =
            smallBody > 0.5
                ? SmallBodyPhotometry(
                      n,
                      l,
                      v)
                : 0.0;

        const float3 color =
            smallBody > 0.5
                ? surfaceColor *
                      particulate *
                      max(g.lighting.w, 0.0)
                : surfaceColor *
                      (0.05 +
                       0.95 * ndl *
                       cloudTransmission) *
                      max(g.lighting.w, 0.0) +
                  glint *
                      max(g.lighting.w, 0.0) *
                      cloudTransmission *
                      float3(1.0, 0.98, 0.94) +
                  ice * 0.025 +
                  (giant > 0.5
                      ? float3(0.0, 0.0, 0.0)
                      : g.emissionAndOpacity.xyz);

        return float4(
            color,
            opacity);
    }

    if (mode >= 2u)
    {
        const float radiusNdc =
            max(g.proxy.y, 0.00025);
        const float2 q =
            p / radiusNdc;
        const float r2 = dot(q, q);

        if (r2 > 1.0)
            discard;

        const float fluxScale =
            max(g.proxy.z, 0.0);
        const float radiometricIntensity =
            max(g.proxy.w, 0.0);

        if (mode == 3u)
        {
            const float r =
                sqrt(max(r2, 0.0));
            const float coreRatio =
                clamp(g.ocean.x, 0.02, 1.0);
            const float coreSigma =
                max(coreRatio * 0.45, 0.01);
            const float core =
                exp(
                    -0.5 *
                    r2 /
                    (coreSigma * coreSigma));

            const float halo =
                1.0 /
                pow(
                    1.0 + 8.0 * r,
                    2.1);

            const float spikeX =
                exp(-abs(q.x) * 48.0) *
                exp(-r * 3.0);
            const float spikeY =
                exp(-abs(q.y) * 48.0) *
                exp(-r * 3.0);
            const float spikes =
                (spikeX + spikeY) * 0.5;

            const float glareStrength =
                max(g.lighting.z, 0.0);

            const float profile =
                core +
                glareStrength *
                    (0.42 * halo +
                     0.08 * spikes);

            const float3 color =
                g.albedoAndRoughness.xyz *
                radiometricIntensity *
                fluxScale *
                profile;

            const float alpha =
                opacity *
                saturate(
                    core +
                    glareStrength *
                        (halo + spikes));

            return float4(color, alpha);
        }

        const float edge =
            saturate((1.0 - r2) * 4.0);

        const float3 color =
            (g.albedoAndRoughness.xyz +
             g.emissionAndOpacity.xyz) *
            fluxScale *
            radiometricIntensity;

        return float4(
            color,
            opacity * edge);
    }

    const float3 radii =
        max(
            g.radiiAndAspect.xyz,
            float3(0.001, 0.001, 0.001));
    const float3 camera =
        g.cameraAndTanHalfFov.xyz;
    const float3 forward =
        normalize(g.forward.xyz);
    const float3 requestedUp =
        normalize(g.up.xyz);
    const float3 right =
        normalize(cross(forward, requestedUp));
    const float3 cameraUp =
        normalize(cross(right, forward));
    const float tanHalf =
        max(g.cameraAndTanHalfFov.w, 0.001);

    const float3 ray =
        normalize(
            forward +
            right *
                (p.x *
                 g.radiiAndAspect.w *
                 tanHalf) -
            cameraUp *
                (p.y * tanHalf));

    const float3 ro = camera / radii;
    const float3 rd = ray / radii;
    const float smallBody =
        g.material.z > 2.5
            ? 1.0
            : 0.0;

    float t = 0.0;
    float3 n = float3(0.0, 0.0, 1.0);

    if (smallBody > 0.5)
    {
        if (!SmallBodyIntersect(
                ro,
                rd,
                radii,
                t,
                n))
            discard;
    }
    else
    {
        const float a = dot(rd, rd);
        const float b = 2.0 * dot(ro, rd);
        const float cc = dot(ro, ro) - 1.0;
        const float disc =
            b * b - 4.0 * a * cc;

        if (disc < 0.0)
            discard;

        t =
            (-b - sqrt(disc)) /
            (2.0 * a);

        if (t < 0.0)
            discard;

        const float3 hit =
            camera + ray * t;

        n =
            normalize(float3(
                hit.x / (radii.x * radii.x),
                hit.y / (radii.y * radii.y),
                hit.z / (radii.z * radii.z)));
    }

    const float3 l =
        normalize(g.lighting.xyz);

    const float ndl =
        saturate(dot(n, l));
    const float giant =
        g.material.z > 1.5
            ? 1.0
            : 0.0;
    const float roughness =
        giant > 0.5
            ? 0.90
            : saturate(g.albedoAndRoughness.w);
    const float ocean =
        giant > 0.5
            ? 0.0
            : saturate(g.material.x);
    const float ice =
        giant > 0.5
            ? 0.0
            : saturate(g.material.y);

    const float diffuse =
        0.045 + 0.955 * ndl;
    const float rim =
        pow(1.0 - saturate(abs(dot(n, -ray))), 4.0);

    const float cloudTransmission =
        giant > 0.5
            ? 1.0
            : saturate(g.material.w);
    const float3 viewDirection =
        normalize(-ray);
    const float glint =
        ocean *
        (1.0 - ice) *
        saturate(g.ocean.w) *
        OceanSpecular(
            n,
            l,
            viewDirection,
            g.ocean.y,
            g.ocean.x,
            g.ocean.z);

    const float3 surfaceColor =
        giant > 0.5
            ? GiantSurfaceColor(n)
            : smallBody > 0.5
                ? SmallBodySurfaceColor(n)
                : g.albedoAndRoughness.xyz;

    const float particulate =
        smallBody > 0.5
            ? SmallBodyPhotometry(
                  n,
                  l,
                  viewDirection)
            : 0.0;

    float3 color =
        smallBody > 0.5
            ? surfaceColor *
                  particulate *
                  max(g.lighting.w, 0.0)
            : surfaceColor *
                  (0.045 +
                   0.955 * ndl *
                   cloudTransmission) *
                  max(g.lighting.w, 0.0) +
              glint *
                  max(g.lighting.w, 0.0) *
                  cloudTransmission *
                  float3(1.0, 0.98, 0.94) +
              ice * 0.025 +
              (giant > 0.5
                  ? float3(0.0, 0.0, 0.0)
                  : g.emissionAndOpacity.xyz);

    if (abs(g.material.z - 1.0) < 0.25)
    {
        const float radiometricIntensity =
            max(g.proxy.w, 0.0);
        const float centerToLimb =
            saturate(abs(dot(n, -ray)));
        color =
            g.albedoAndRoughness.xyz *
            radiometricIntensity *
            (0.58 + 0.42 * centerToLimb);
    }

    return float4(
        color,
        opacity);
}
)";

constexpr const char* kSurfacePs = R"(
struct Constants
{
    float4 radiiAndAspect;
    float4 cameraAndTanHalfFov;
    float4 forward;
    float4 up;
    float4 albedoAndRoughness;
    float4 material;
    float4 emissionAndOpacity;
    float4 proxy;
    float4 lighting;
    float4 ocean;
};
[[vk::push_constant]] Constants g;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct SurfaceOutputs
{
    float4 baseRoughness : SV_Target0;
    float4 normalMetallic : SV_Target1;
    float4 emissionClass : SV_Target2;
};

float EncodeSurfaceMeta(float surfaceClass, float representation)
{
    return surfaceClass + representation / 16.0;
}

uint SmallBodySurfaceHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float SmallBodySurfaceHash01(uint value)
{
    return
        (float)(SmallBodySurfaceHash(value) & 0x00ffffffu) /
        16777215.0;
}

float SmallBodySurfaceNoise(
    float3 p,
    float scale,
    uint seed)
{
    int3 q =
        (int3)floor(
            p *
            max(scale, 1.0) *
            4096.0);

    uint h =
        SmallBodySurfaceHash(
            seed ^ asuint(q.x));
    h =
        SmallBodySurfaceHash(
            h ^ asuint(q.y));
    h =
        SmallBodySurfaceHash(
            h ^ asuint(q.z));

    return
        (float)(h & 0x00ffffffu) /
        16777215.0;
}

float SmallBodySurfaceFractal(
    float3 p,
    float scale,
    uint seed)
{
    float sum = 0.0;
    float weight = 0.0;
    float amplitude = 1.0;
    float frequency = scale;

    [unroll]
    for (uint octave = 0u; octave < 4u; ++octave)
    {
        sum +=
            SmallBodySurfaceNoise(
                p,
                frequency,
                seed + octave * 0x9e3779b9u) *
            amplitude;
        weight += amplitude;
        amplitude *= 0.5;
        frequency *= 2.07;
    }

    return sum / max(weight, 1e-6);
}

float2 SmallBodySurfaceUnpack01(uint packed)
{
    return float2(
        (float)(packed & 0xffffu) / 65535.0,
        (float)((packed >> 16u) & 0xffffu) / 65535.0);
}

float SmallBodySurfaceCraterRelief(float3 d)
{
    const float density = saturate(g.ocean.z);
    const float2 craterShape =
        SmallBodySurfaceUnpack01(
            asuint(g.forward.w));
    const float craterDepth =
        craterShape.x * 0.5;
    const float craterRimStrength =
        craterShape.y * 0.5;
    const uint seed =
        asuint(g.material.w);
    const uint count =
        2u + (uint)round(density * 10.0);

    float relief = 0.0;

    [unroll]
    for (uint i = 0u; i < 12u; ++i)
    {
        if (i >= count)
            continue;

        const float z =
            SmallBodySurfaceHash01(
                seed + i * 41u + 7u) *
            2.0 - 1.0;
        const float azimuth =
            SmallBodySurfaceHash01(
                seed + i * 73u + 19u) *
            6.28318530718;
        const float radial =
            sqrt(max(1.0 - z * z, 0.0));
        const float3 center =
            float3(
                radial * cos(azimuth),
                z,
                radial * sin(azimuth));

        const float angle =
            acos(
                clamp(
                    dot(d, center),
                    -1.0,
                    1.0));
        const float radius =
            0.035 +
            0.11 *
            SmallBodySurfaceHash01(
                seed + i * 137u + 29u);
        const float q =
            angle /
            max(radius, 1e-5);

        const float bowl =
            q < 1.0
                ? -(1.0 - q * q)
                : 0.0;
        const float rim =
            smoothstep(1.35, 1.0, q) -
            smoothstep(1.0, 0.86, q);

        relief +=
            bowl * craterDepth +
            rim * craterRimStrength;
    }

    return relief;
}

float SmallBodySurfaceRadiusScale(
    float3 direction)
{
    const float3 d =
        normalize(direction);
    const float3 axes =
        max(
            float3(
                g.material.x,
                g.material.y,
                g.proxy.z),
            float3(0.05, 0.05, 0.05));

    const float ellipsoid =
        rsqrt(
            dot(
                d * d,
                1.0 / (axes * axes)));

    const uint seed =
        asuint(g.material.w);
    const float irregularity =
        saturate(g.ocean.x);
    const float lobeStrength =
        saturate(g.ocean.y);

    const float broad =
        (SmallBodySurfaceFractal(
            d,
            2.1,
            seed ^ 0x53484142u) -
         0.5) *
        2.0;
    const float medium =
        (SmallBodySurfaceFractal(
            d,
            5.7,
            seed ^ 0x5348414du) -
         0.5) *
        2.0;
    const float lobe =
        sin(
            atan2(d.z, d.x) * 2.0 +
            d.y * 2.7 +
            SmallBodySurfaceHash01(seed) * 6.0) *
        lobeStrength;

    return max(
        0.18,
        ellipsoid *
        (1.0 +
         irregularity *
             (0.68 * broad +
              0.32 * medium) +
         lobe +
         SmallBodySurfaceCraterRelief(d)));
}

float SmallBodySurfaceField(
    float3 q)
{
    const float radial =
        length(q);

    if (radial <= 1e-6)
        return -1.0;

    return
        radial -
        SmallBodySurfaceRadiusScale(
            q / radial);
}

float3 SmallBodySurfaceNormal(
    float3 q,
    float3 radii)
{
    const float e = 0.0025;
    const float3 dx = float3(e, 0.0, 0.0);
    const float3 dy = float3(0.0, e, 0.0);
    const float3 dz = float3(0.0, 0.0, e);

    const float3 gradient =
        float3(
            SmallBodySurfaceField(q + dx) -
                SmallBodySurfaceField(q - dx),
            SmallBodySurfaceField(q + dy) -
                SmallBodySurfaceField(q - dy),
            SmallBodySurfaceField(q + dz) -
                SmallBodySurfaceField(q - dz));

    return normalize(
        gradient /
        max(
            radii,
            float3(0.001, 0.001, 0.001)));
}

bool SmallBodySurfaceIntersect(
    float3 ro,
    float3 rd,
    float3 radii,
    out float tHit,
    out float3 normal)
{
    const float3 axes =
        max(
            float3(
                g.material.x,
                g.material.y,
                g.proxy.z),
            float3(0.05, 0.05, 0.05));

    const float bound =
        max(
            axes.x,
            max(axes.y, axes.z)) *
        (1.0 +
         saturate(g.ocean.x) +
         saturate(g.ocean.y) +
         0.55);

    const float a = dot(rd, rd);
    const float b = 2.0 * dot(ro, rd);
    const float cc =
        dot(ro, ro) -
        bound * bound;
    const float discriminant =
        b * b - 4.0 * a * cc;

    if (discriminant < 0.0)
        return false;

    const float root =
        sqrt(discriminant);
    const float enter =
        max(
            (-b - root) /
                max(2.0 * a, 1e-6),
            0.0);
    const float exit =
        (-b + root) /
        max(2.0 * a, 1e-6);

    if (exit <= enter)
        return false;

    float previousT = enter;
    float previousField =
        SmallBodySurfaceField(
            ro + rd * previousT);

    [loop]
    for (uint step = 1u; step <= 28u; ++step)
    {
        const float currentT =
            lerp(
                enter,
                exit,
                (float)step / 28.0);
        const float currentField =
            SmallBodySurfaceField(
                ro + rd * currentT);

        if (currentField <= 0.0 &&
            previousField > 0.0)
        {
            float low = previousT;
            float high = currentT;

            [unroll]
            for (uint refine = 0u;
                 refine < 8u;
                 ++refine)
            {
                const float mid =
                    0.5 * (low + high);
                const float midField =
                    SmallBodySurfaceField(
                        ro + rd * mid);

                if (midField > 0.0)
                    low = mid;
                else
                    high = mid;
            }

            tHit = high;
            const float3 q =
                ro + rd * tHit;
            normal =
                SmallBodySurfaceNormal(
                    q,
                    radii);
            return true;
        }

        previousT = currentT;
        previousField = currentField;
    }

    return false;
}

SurfaceOutputs main(VSOutput input)
{
    const uint mode = (uint)round(g.proxy.x);
    const float2 p = input.uv;
    float3 n = float3(0.0, 0.0, 1.0);

    const float smallBody =
        g.material.z > 2.5
            ? 1.0
            : 0.0;

    if (mode == 1u || mode >= 2u)
    {
        const float radiusNdc =
            max(g.proxy.y, 0.00025);
        const float2 q =
            p / radiusNdc;
        const float r2 =
            dot(q, q);

        if (smallBody > 0.5 &&
            mode == 1u)
        {
            const float r =
                sqrt(max(r2, 0.0));
            const float3 edgeDirection =
                normalize(
                    float3(
                        q.x,
                        -q.y,
                        sqrt(
                            max(
                                1.0 -
                                min(r2, 1.0),
                                0.0))));

            const float radiusScale =
                SmallBodySurfaceRadiusScale(
                    edgeDirection);

            if (r > radiusScale)
                discard;

            const float2 sq =
                q /
                max(radiusScale, 0.05);
            const float sr2 =
                dot(sq, sq);

            if (sr2 > 1.0)
                discard;

            n =
                normalize(
                    float3(
                        sq.x,
                        -sq.y,
                        sqrt(
                            max(
                                1.0 - sr2,
                                0.0))));
        }
        else
        {
            if (r2 > 1.0)
                discard;

            const float z =
                sqrt(
                    max(
                        1.0 - r2,
                        0.0));
            n =
                normalize(
                    float3(
                        q.x,
                        -q.y,
                        z));
        }
    }
    else
    {
        const float3 radii =
            max(
                g.radiiAndAspect.xyz,
                float3(0.001, 0.001, 0.001));
        const float3 camera =
            g.cameraAndTanHalfFov.xyz;
        const float3 forward =
            normalize(g.forward.xyz);
        const float3 requestedUp =
            normalize(g.up.xyz);
        const float3 right =
            normalize(
                cross(
                    forward,
                    requestedUp));
        const float3 cameraUp =
            normalize(
                cross(
                    right,
                    forward));
        const float tanHalf =
            max(
                g.cameraAndTanHalfFov.w,
                0.001);

        const float3 ray =
            normalize(
                forward +
                right *
                    (p.x *
                     g.radiiAndAspect.w *
                     tanHalf) -
                cameraUp *
                    (p.y * tanHalf));

        const float3 ro =
            camera / radii;
        const float3 rd =
            ray / radii;

        if (smallBody > 0.5)
        {
            float t = 0.0;

            if (!SmallBodySurfaceIntersect(
                    ro,
                    rd,
                    radii,
                    t,
                    n))
                discard;
        }
        else
        {
            const float a =
                dot(rd, rd);
            const float b =
                2.0 * dot(ro, rd);
            const float cc =
                dot(ro, ro) - 1.0;
            const float disc =
                b * b -
                4.0 * a * cc;

            if (disc < 0.0)
                discard;

            const float t =
                (-b - sqrt(disc)) /
                (2.0 * a);

            if (t < 0.0)
                discard;

            const float3 hit =
                camera + ray * t;

            n =
                normalize(
                    float3(
                        hit.x /
                            (radii.x * radii.x),
                        hit.y /
                            (radii.y * radii.y),
                        hit.z /
                            (radii.z * radii.z)));
        }
    }

    SurfaceOutputs output;
    output.baseRoughness =
        float4(
            max(g.albedoAndRoughness.xyz, 0.0),
            saturate(g.albedoAndRoughness.w));
    output.normalMetallic =
        float4(
            n,
            0.0);
    float3 surfaceEmission =
        smallBody > 0.5
            ? float3(0.0, 0.0, 0.0)
            : max(
                  g.emissionAndOpacity.xyz,
                  0.0);

    if (abs(g.material.z - 1.0) < 0.25)
    {
        // Radiative bodies are their own light source. Preserve the same
        // resolved scene-radiance scale used by the visual far-body path so
        // the shared lighting/GI pipeline sees the source instead of a dim
        // material proxy.
        surfaceEmission =
            max(g.albedoAndRoughness.xyz, 0.0) *
            max(g.proxy.w, 0.0);
    }

    output.emissionClass =
        float4(
            surfaceEmission,
            EncodeSurfaceMeta(
                6.0,
                g.proxy.x));
    return output;
}
)";

constexpr const char* kCachedPs = R"(
[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_disc;

[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sampler;

struct Constants
{
    float4 proxy;
};
[[vk::push_constant]] Constants g;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(VSOutput input) : SV_Target0
{
    const float radiusNdc =
        max(g.proxy.x, 0.00025);
    const float2 q =
        input.uv / radiusNdc;

    if (dot(q, q) > 1.0)
        discard;

    const float2 uv =
        float2(
            q.x * 0.5 + 0.5,
            0.5 - q.y * 0.5);

    float4 color =
        g_disc.Sample(
            g_sampler,
            uv);

    color.a *=
        saturate(g.proxy.y);

    return color;
}
)";
} // namespace

AppearanceSummary SummarizeAppearance(
    const celestial_appearance::PlanetaryAppearanceProduct& appearance)
{
    if (appearance.texels.empty())
    {
        throw std::invalid_argument(
            "Cannot summarize an empty planetary appearance product.");
    }

    math::Double3 albedo{};
    math::Double3 emission{};
    f64 roughness = 0.0;
    f64 ocean = 0.0;
    f64 ice = 0.0;
    f64 transmittance = 0.0;

    for (const auto& texel :
         appearance.texels)
    {
        albedo =
            albedo +
            math::Double3{
                texel.albedoLinear.x,
                texel.albedoLinear.y,
                texel.albedoLinear.z
            };
        emission =
            emission +
            math::Double3{
                texel.emissionLinear.x,
                texel.emissionLinear.y,
                texel.emissionLinear.z
            };
        roughness += texel.roughness;
        ocean += texel.oceanMask;
        ice += texel.iceMask;
        transmittance +=
            texel.directLightTransmittance;
    }

    const f64 inv =
        1.0 /
        static_cast<f64>(
            appearance.texels.size());

    return {
        .albedoLinear = {
            static_cast<f32>(albedo.x * inv),
            static_cast<f32>(albedo.y * inv),
            static_cast<f32>(albedo.z * inv)
        },
        .roughness =
            static_cast<f32>(
                roughness * inv),
        .oceanFraction =
            static_cast<f32>(
                ocean * inv),
        .iceFraction =
            static_cast<f32>(
                ice * inv),
        .directLightTransmittance =
            static_cast<f32>(
                transmittance * inv),
        .emissionLinear = {
            static_cast<f32>(emission.x * inv),
            static_cast<f32>(emission.y * inv),
            static_cast<f32>(emission.z * inv)
        }
    };
}

CachedDiscProduct BuildCachedDisc(
    const celestial_appearance::PlanetaryAppearanceProduct& appearance,
    const CachedDiscConfig& config)
{
    if (config.resolution < 8U ||
        appearance.texels.empty())
    {
        throw std::invalid_argument(
            "Cached disc input is invalid.");
    }

    CachedDiscProduct result;
    result.resolution =
        config.resolution;
    result.appearanceFingerprint =
        appearance.fingerprint;
    result.fingerprint =
        DiscFingerprint(
            appearance,
            config);
    result.rgba16.resize(
        static_cast<std::size_t>(
            config.resolution) *
        config.resolution *
        4U,
        0U);

    const math::Double3 light =
        math::Normalize(
            math::Double3{
                0.55, 0.72, 0.48});

    for (u32 y = 0;
         y < config.resolution;
         ++y)
    {
        for (u32 x = 0;
             x < config.resolution;
             ++x)
        {
            const f64 px =
                (2.0 *
                 (static_cast<f64>(x) + 0.5) /
                 static_cast<f64>(config.resolution)) -
                1.0;
            const f64 py =
                1.0 -
                (2.0 *
                 (static_cast<f64>(y) + 0.5) /
                 static_cast<f64>(config.resolution));

            const f64 r2 =
                px * px + py * py;

            if (r2 > 1.0)
            {
                continue;
            }

            const auto direction =
                DiscDirection(px, py);

            const auto& sample =
                SampleAppearance(
                    appearance,
                    direction);

            const f32 ndl =
                static_cast<f32>(
                    std::clamp(
                        math::Dot(
                            direction,
                            light),
                        0.0,
                        1.0));

            math::Float3 color =
                sample.albedoLinear *
                    (0.05F +
                     0.95F * ndl) +
                sample.emissionLinear;

            const std::size_t offset =
                (static_cast<std::size_t>(y) *
                     config.resolution +
                 x) *
                4U;

            result.rgba16[offset] =
                FloatToHalfBits(color.x);
            result.rgba16[offset + 1U] =
                FloatToHalfBits(color.y);
            result.rgba16[offset + 2U] =
                FloatToHalfBits(color.z);

            const f64 edge =
                std::clamp(
                    (1.0 - r2) *
                        static_cast<f64>(
                            config.resolution) *
                        0.5,
                    0.0,
                    1.0);

            result.rgba16[offset + 3U] =
                FloatToHalfBits(
                    static_cast<f32>(edge));
        }
    }

    return result;
}

GpuCachedDiscProduct::GpuCachedDiscProduct(
    rhi::Device& device,
    const CachedDiscProduct& product)
    : fingerprint_(product.fingerprint)
{
    if (product.resolution == 0U ||
        product.rgba16.size() !=
            static_cast<std::size_t>(
                product.resolution) *
                product.resolution *
                4U)
    {
        throw std::invalid_argument(
            "GPU cached disc product is invalid.");
    }

    staging_ =
        device.CreateBuffer({
            .sizeBytes =
                static_cast<u64>(
                    product.rgba16.size() *
                    sizeof(u16)),
            .usage =
                rhi::BufferUsage::Generic,
            .memory =
                rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::CopySource
        });

    texture_ =
        device.CreateTexture({
            .width = product.resolution,
            .height = product.resolution,
            .format =
                rhi::TextureFormat::RGBA16_Float,
            .initialState =
                rhi::ResourceState::
                    CopyDestination
        });

    if (!staging_ || !texture_)
    {
        throw std::runtime_error(
            "Failed to allocate cached disc GPU resources.");
    }

    std::memcpy(
        staging_->Map(),
        product.rgba16.data(),
        product.rgba16.size() *
            sizeof(u16));
    staging_->Unmap();
}

void GpuCachedDiscProduct::EnsureUploaded(
    rhi::CommandList& commands)
{
    if (uploaded_)
    {
        return;
    }

    commands.CopyBufferToTexture(
        *staging_,
        0,
        *texture_);

    commands.Transition(
        *texture_,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::ShaderResource);

    uploaded_ = true;
}

rhi::Texture&
GpuCachedDiscProduct::Texture() noexcept
{
    return *texture_;
}

u64 GpuCachedDiscProduct::Fingerprint() const noexcept
{
    return fingerprint_;
}

FarBodyRenderer::FarBodyRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto vs =
        compiler.Compile({
            .source = kQuadVs,
            .entryPoint = "main",
            .stage = shader::Stage::Vertex,
            .debug = false
        });

    const auto analyticPs =
        compiler.Compile({
            .source = kAnalyticPs,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

    const auto cachedPs =
        compiler.Compile({
            .source = kCachedPs,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

    const auto surfacePs =
        compiler.Compile({
            .source = kSurfacePs,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

    analyticPipeline_ =
        device.CreateGraphicsPipeline({
            .vertexShader = {
                .data = vs.bytecode.data(),
                .size = vs.bytecode.size()
            },
            .pixelShader = {
                .data =
                    analyticPs.bytecode.data(),
                .size =
                    analyticPs.bytecode.size()
            },
            .vertexAttributes = {},
            .vertexStrideBytes = 0,
            .pushConstantDwords = 40,
            .topology =
                rhi::PrimitiveTopology::
                    TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Alpha,
            .depthTest = false,
            .depthWrite = false,
            .colorAttachmentFormats = {
                rhi::TextureFormat::RGBA16_Float
            },
            .colorAttachmentCount = 1U
        });

    cachedPipeline_ =
        device.CreateGraphicsPipeline({
            .vertexShader = {
                .data = vs.bytecode.data(),
                .size = vs.bytecode.size()
            },
            .pixelShader = {
                .data =
                    cachedPs.bytecode.data(),
                .size =
                    cachedPs.bytecode.size()
            },
            .vertexAttributes = {},
            .vertexStrideBytes = 0,
            .pushConstantDwords = 4,
            .sampledTextures = 1,
            .topology =
                rhi::PrimitiveTopology::
                    TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Alpha,
            .depthTest = false,
            .depthWrite = false,
            .colorAttachmentFormats = {
                rhi::TextureFormat::RGBA16_Float
            },
            .colorAttachmentCount = 1U
        });

    surfacePipeline_ =
        device.CreateGraphicsPipeline({
            .vertexShader = {
                .data = vs.bytecode.data(),
                .size = vs.bytecode.size()
            },
            .pixelShader = {
                .data = surfacePs.bytecode.data(),
                .size = surfacePs.bytecode.size()
            },
            .vertexAttributes = {},
            .vertexStrideBytes = 0,
            .pushConstantDwords = 40,
            .topology =
                rhi::PrimitiveTopology::
                    TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Opaque,
            .depthTest = false,
            .depthWrite = false,
            .colorAttachmentFormats = {
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float
            },
            .colorAttachmentCount = 3U
        });

}

void FarBodyRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    const FarBodyDraw& draw,
    GpuCachedDiscProduct* cachedDisc)
{
    if (width == 0U ||
        height == 0U ||
        draw.opacity <= 0.0F)
    {
        return;
    }

    const auto ellipsoid =
        AsEllipsoid(draw.shape);

    const f64 scale =
        std::max({
            ellipsoid.radiiMeters.x,
            ellipsoid.radiiMeters.y,
            ellipsoid.radiiMeters.z,
            1.0
        });

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    const auto packUnitPair =
        [](const f32 a, const f32 b)
        {
            const auto quantize =
                [](const f32 value)
                {
                    return static_cast<u32>(
                        std::lround(
                            std::clamp(
                                value,
                                0.0F,
                                1.0F) *
                            65535.0F));
                };

            return
                quantize(a) |
                (quantize(b) << 16U);
        };

    const f64 minimumRasterRadiusPixels =
        0.5;

    const bool stellarPointProxy =
        draw.stellar &&
        draw.representation ==
            celestial_representation::
                Representation::
                    StellarPointProxy;

    const f64 stellarCoreRadiusPixels =
        std::max(
            draw.projectedRadiusPixels,
            minimumRasterRadiusPixels);

    const f64 rasterRadiusPixels =
        stellarPointProxy
            ? std::max(
                  static_cast<f64>(
                      draw.stellarGlareRadiusPixels),
                  minimumRasterRadiusPixels)
            : std::max(
                  draw.projectedRadiusPixels,
                  minimumRasterRadiusPixels);

    const f32 radiusNdc =
        static_cast<f32>(
            2.0 *
            rasterRadiusPixels /
            static_cast<f64>(
                std::max(
                    height,
                    1U)));

    const f32 pointFluxScale =
        stellarPointProxy
            ? static_cast<f32>(
                  1.0 /
                  std::max(
                      rasterRadiusPixels *
                          rasterRadiusPixels,
                      0.25))
            : static_cast<f32>(
                  std::clamp(
                      (draw.projectedRadiusPixels *
                       draw.projectedRadiusPixels) /
                          (rasterRadiusPixels *
                           rasterRadiusPixels),
                      0.0,
                      1.0));

    const f32 stellarCoreToGlare =
        stellarPointProxy
            ? static_cast<f32>(
                  std::clamp(
                      stellarCoreRadiusPixels /
                          rasterRadiusPixels,
                      0.02,
                      1.0))
            : 1.0F;

    commands.SetRenderTarget(target);
    commands.SetViewport({
        .x = 0.0F,
        .y = 0.0F,
        .width = static_cast<f32>(width),
        .height = static_cast<f32>(height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F
    });
    commands.SetScissor({
        .left = 0,
        .top = 0,
        .right = static_cast<i32>(width),
        .bottom = static_cast<i32>(height)
    });

    if (draw.representation ==
            celestial_representation::
                Representation::
                    CachedDiscImpostor &&
        cachedDisc != nullptr &&
        !draw.giantEnabled &&
        !draw.smallBodyEnabled &&
        !(draw.oceanEnabled &&
          draw.appearance.oceanFraction > 0.0F &&
          draw.incidentLightScale > 0.0F))
    {
        cachedDisc->EnsureUploaded(
            commands);

        const std::array<u32, 4>
            constants{
                bits(radiusNdc),
                bits(std::clamp(
                    draw.opacity,
                    0.0F,
                    1.0F)),
                0U,
                0U
            };

        commands.SetGraphicsPipeline(
            *cachedPipeline_);
        commands.SetGraphicsConstants(
            constants);
        commands.SetGraphicsTexture(
            0,
            cachedDisc->Texture());
        commands.Draw(6);
        return;
    }

    u32 mode = 0U;

    switch (draw.representation)
    {
    case celestial_representation::
        Representation::SmoothGlobe:
        mode = 0U;
        break;
    case celestial_representation::
        Representation::
            AnalyticDiscImpostor:
    case celestial_representation::
        Representation::
            CachedDiscImpostor:
        mode = 1U;
        break;
    case celestial_representation::
        Representation::PointProxy:
        mode = 2U;
        break;
    case celestial_representation::
        Representation::StellarPointProxy:
        mode = 3U;
        break;
    default:
        mode = 0U;
        break;
    }

    if (draw.stellar &&
        mode == 0U)
    {
        mode = 1U;
    }

    const f32 tanHalfFov =
        std::tan(
            draw.camera.verticalFovRadians *
            0.5F);

    f32 proxyRadiometricIntensity =
        std::max(
            draw.radiometricIntensity,
            0.0F);

    if (!draw.stellar &&
        (mode == 2U || mode == 3U) &&
        math::LengthSquared(
            draw.camera.localPositionMeters) >
            1.0e-20)
    {
        const auto observerDirection =
            math::Normalize(
                draw.camera.
                    localPositionMeters);

        const math::Double3 lightDirection{
            draw.lightDirectionBody.x,
            draw.lightDirectionBody.y,
            draw.lightDirectionBody.z
        };

        if (math::LengthSquared(
                lightDirection) >
            1.0e-20)
        {
            const f64 phaseAngle =
                std::acos(
                    std::clamp(
                        math::Dot(
                            observerDirection,
                            math::Normalize(
                                lightDirection)),
                        -1.0,
                        1.0));

            f64 phaseResponse =
                celestial_lighting::
                    LambertPhase(
                        phaseAngle);

            if (draw.smallBodyEnabled)
            {
                const f64 opposition =
                    1.0 +
                    std::max(
                        static_cast<f64>(
                            draw.smallBodyOppositionStrength),
                        0.0) /
                    (1.0 +
                     std::tan(
                         phaseAngle * 0.5) /
                     std::max(
                         static_cast<f64>(
                             draw.smallBodyOppositionWidthRadians),
                         1.0e-6));

                const f64 roughnessAttenuation =
                    std::exp(
                        -0.35 *
                        static_cast<f64>(
                            draw.smallBodyMacroscopicRoughnessRadians) *
                        static_cast<f64>(
                            draw.smallBodyMacroscopicRoughnessRadians) *
                        std::sin(phaseAngle));

                phaseResponse *=
                    opposition *
                    roughnessAttenuation;
            }

            proxyRadiometricIntensity =
                static_cast<f32>(
                    std::max(
                        draw.incidentLightScale,
                        0.0F) *
                    phaseResponse);
        }
    }

    const u32 packedSmallBodyCrater =
        packUnitPair(
            std::clamp(
                draw.smallBodyCraterDepth / 0.5F,
                0.0F,
                1.0F),
            std::clamp(
                draw.smallBodyCraterRimStrength / 0.5F,
                0.0F,
                1.0F));

    const u32 packedSmallBodyMaterial =
        packUnitPair(
            draw.smallBodySingleScatteringAlbedo,
            draw.smallBodyColorVariation);

    const std::array<u32, 40>
        constants{
            bits(static_cast<f32>(
                ellipsoid.radiiMeters.x /
                scale)),
            bits(static_cast<f32>(
                ellipsoid.radiiMeters.y /
                scale)),
            bits(static_cast<f32>(
                ellipsoid.radiiMeters.z /
                scale)),
            bits(static_cast<f32>(width) /
                 static_cast<f32>(height)),

            bits(static_cast<f32>(
                draw.camera.localPositionMeters.x /
                scale)),
            bits(static_cast<f32>(
                draw.camera.localPositionMeters.y /
                scale)),
            bits(static_cast<f32>(
                draw.camera.localPositionMeters.z /
                scale)),
            bits(tanHalfFov),

            bits(draw.camera.forward.x),
            bits(draw.camera.forward.y),
            bits(draw.camera.forward.z),
            draw.smallBodyEnabled
                ? packedSmallBodyCrater
                : draw.giantEnabled
                    ? bits(draw.giantDepthContrast)
                    : 0U,

            bits(draw.camera.up.x),
            bits(draw.camera.up.y),
            bits(draw.camera.up.z),
            draw.smallBodyEnabled
                ? packedSmallBodyMaterial
                : draw.giantEnabled
                    ? bits(draw.giantTurbulenceStrength)
                    : 0U,

            bits(draw.stellar
                ? draw.stellarColorLinear.x
                : draw.giantEnabled
                    ? draw.giantBaseColorLinear.x
                    : draw.appearance.albedoLinear.x),
            bits(draw.stellar
                ? draw.stellarColorLinear.y
                : draw.giantEnabled
                    ? draw.giantBaseColorLinear.y
                    : draw.appearance.albedoLinear.y),
            bits(draw.stellar
                ? draw.stellarColorLinear.z
                : draw.giantEnabled
                    ? draw.giantBaseColorLinear.z
                    : draw.appearance.albedoLinear.z),
            bits(draw.stellar
                ? std::clamp(
                      draw.stellarLimbDarkening,
                      0.0F,
                      1.0F)
                : draw.giantEnabled
                    ? std::max(
                          draw.giantZonalShear,
                          0.0F)
                    : draw.smallBodyEnabled
                        ? std::max(
                              draw.smallBodyMacroscopicRoughnessRadians,
                              0.0F)
                        : draw.appearance.roughness),

            bits(draw.stellar
                ? std::clamp(
                      draw.stellarGranulationStrength,
                      0.0F,
                      1.0F)
                : draw.giantEnabled
                    ? std::max(
                          draw.giantBandFrequency,
                          1.0F)
                    : draw.smallBodyEnabled
                        ? std::max(
                              draw.smallBodyAxisScale.x,
                              0.05F)
                        : draw.appearance.oceanFraction),
            bits(draw.stellar
                ? std::clamp(
                      draw.stellarActivityLevel,
                      0.0F,
                      1.0F)
                : draw.giantEnabled
                    ? std::clamp(
                          draw.giantBandStrength,
                          0.0F,
                          1.0F)
                    : draw.smallBodyEnabled
                        ? std::max(
                              draw.smallBodyAxisScale.y,
                              0.05F)
                        : draw.appearance.iceFraction),
            bits(draw.smallBodyEnabled
                ? 3.0F
                : draw.giantEnabled
                    ? 2.0F
                    : draw.stellar
                        ? 1.0F
                        : 0.0F),
            draw.smallBodyEnabled
                ? draw.smallBodySeed
                : draw.giantEnabled
                    ? draw.giantSeed
                    : draw.stellar
                        ? draw.stellarActivitySeed
                        : bits(std::clamp(
                              draw.appearance.directLightTransmittance,
                              0.0F,
                              1.0F)),

            bits(draw.stellar
                ? std::max(
                      draw.stellarGranulationScale,
                      1.0F)
                : draw.giantEnabled
                    ? draw.giantBandColorLinear.x
                    : draw.smallBodyEnabled
                        ? draw.smallBodyFreshMaterialColorLinear.x
                        : draw.appearance.emissionLinear.x),
            bits(draw.stellar
                ? std::max(
                      draw.stellarChromosphereStrength,
                      0.0F)
                : draw.giantEnabled
                    ? draw.giantBandColorLinear.y
                    : draw.smallBodyEnabled
                        ? draw.smallBodyFreshMaterialColorLinear.y
                        : draw.appearance.emissionLinear.y),
            bits(draw.stellar
                ? std::max(
                      draw.stellarChromosphereExtent,
                      0.0F)
                : draw.giantEnabled
                    ? draw.giantBandColorLinear.z
                    : draw.smallBodyEnabled
                        ? draw.smallBodyFreshMaterialColorLinear.z
                        : draw.appearance.emissionLinear.z),
            bits(std::clamp(
                draw.opacity,
                0.0F,
                1.0F)),

            bits(static_cast<f32>(mode)),
            bits(radiusNdc),
            bits(draw.smallBodyEnabled &&
                     mode <= 1U
                ? std::max(
                      draw.smallBodyAxisScale.z,
                      0.05F)
                : draw.giantEnabled
                    ? std::clamp(
                          draw.giantStormStrength,
                          0.0F,
                          1.0F)
                    : pointFluxScale),
            bits(draw.smallBodyEnabled &&
                     mode <= 1U
                ? std::max(
                      draw.smallBodyOppositionStrength,
                      0.0F)
                : draw.giantEnabled
                    ? std::max(
                          draw.giantStormScale,
                          0.25F)
                    : proxyRadiometricIntensity),

            bits(draw.stellar
                ? std::max(
                      draw.stellarCoronaStrength,
                      0.0F)
                : draw.lightDirectionBody.x),
            bits(draw.stellar
                ? std::max(
                      draw.stellarCoronaExtent,
                      0.0F)
                : draw.lightDirectionBody.y),
            bits(draw.stellar
                ? std::max(
                      draw.stellarGlareStrength,
                      0.0F)
                : draw.lightDirectionBody.z),
            bits(draw.stellar
                ? std::max(
                      draw.stellarGlareRadiusPixels,
                      0.5F)
                : std::max(
                      draw.incidentLightScale,
                      0.0F)),

            bits(draw.stellar
                ? stellarCoreToGlare
                : draw.giantEnabled
                    ? draw.giantPolarColorLinear.x
                    : draw.smallBodyEnabled
                        ? std::clamp(
                              draw.smallBodyIrregularity,
                              0.0F,
                              1.0F)
                        : std::max(
                              draw.oceanRefractiveIndex,
                              1.0F)),
            bits(draw.stellar
                ? 0.0F
                : draw.giantEnabled
                    ? draw.giantPolarColorLinear.y
                    : draw.smallBodyEnabled
                        ? std::clamp(
                              draw.smallBodyLargeLobeStrength,
                              0.0F,
                              1.0F)
                        : std::clamp(
                              draw.oceanRoughness,
                              0.01F,
                              1.0F)),
            bits(draw.stellar
                ? 0.0F
                : draw.giantEnabled
                    ? draw.giantPolarColorLinear.z
                    : draw.smallBodyEnabled
                        ? std::clamp(
                              draw.smallBodyCraterDensity,
                              0.0F,
                              1.0F)
                        : std::max(
                              draw.oceanGlintStrength,
                              0.0F)),
            bits(draw.stellar
                ? 0.0F
                : draw.giantEnabled
                    ? std::clamp(
                          draw.giantPolarStrength,
                          0.0F,
                          1.0F)
                    : draw.smallBodyEnabled
                        ? std::max(
                              draw.smallBodyOppositionWidthRadians,
                              1.0e-6F)
                        : (draw.oceanEnabled
                            ? 1.0F
                            : 0.0F))
        };

    commands.SetGraphicsPipeline(
        *analyticPipeline_);
    commands.SetGraphicsConstants(
        constants);
    commands.Draw(6);
}

void FarBodyRenderer::DrawSurfaceData(
    rhi::CommandList& commands,
    rhi::Texture& surfaceBaseRoughness,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& surfaceEmissionClass,
    const u32 width,
    const u32 height,
    const FarBodyDraw& draw)
{
    if (width == 0U ||
        height == 0U ||
        draw.opacity <= 0.0F)
    {
        return;
    }

    const auto ellipsoid =
        AsEllipsoid(draw.shape);

    const f64 scale =
        std::max({
            ellipsoid.radiiMeters.x,
            ellipsoid.radiiMeters.y,
            ellipsoid.radiiMeters.z,
            1.0
        });

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    const auto packUnitPair =
        [](const f32 a, const f32 b)
        {
            const auto quantize =
                [](const f32 value)
                {
                    return static_cast<u32>(
                        std::lround(
                            std::clamp(
                                value,
                                0.0F,
                                1.0F) *
                            65535.0F));
                };

            return
                quantize(a) |
                (quantize(b) << 16U);
        };

    const f64 minimumRasterRadiusPixels = 0.5;
    const f64 rasterRadiusPixels =
        std::max(
            draw.projectedRadiusPixels,
            minimumRasterRadiusPixels);
    const f32 radiusNdc =
        static_cast<f32>(
            2.0 * rasterRadiusPixels /
            static_cast<f64>(
                std::max(height, 1U)));

    u32 mode = 0U;
    switch (draw.representation)
    {
    case celestial_representation::Representation::SmoothGlobe:
        mode = 0U;
        break;
    case celestial_representation::Representation::AnalyticDiscImpostor:
    case celestial_representation::Representation::CachedDiscImpostor:
        mode = 1U;
        break;
    case celestial_representation::Representation::PointProxy:
        mode = 2U;
        break;
    case celestial_representation::Representation::StellarPointProxy:
        mode = 3U;
        break;
    default:
        mode = 0U;
        break;
    }

    const f32 tanHalfFov =
        std::tan(
            draw.camera.verticalFovRadians *
            0.5F);

    const std::array<u32, 40> constants{
        bits(static_cast<f32>(
            ellipsoid.radiiMeters.x / scale)),
        bits(static_cast<f32>(
            ellipsoid.radiiMeters.y / scale)),
        bits(static_cast<f32>(
            ellipsoid.radiiMeters.z / scale)),
        bits(static_cast<f32>(width) /
             static_cast<f32>(height)),

        bits(static_cast<f32>(
            draw.camera.localPositionMeters.x / scale)),
        bits(static_cast<f32>(
            draw.camera.localPositionMeters.y / scale)),
        bits(static_cast<f32>(
            draw.camera.localPositionMeters.z / scale)),
        bits(tanHalfFov),

        bits(draw.camera.forward.x),
        bits(draw.camera.forward.y),
        bits(draw.camera.forward.z),
        0U,

        bits(draw.camera.up.x),
        bits(draw.camera.up.y),
        bits(draw.camera.up.z),
        0U,

        bits(draw.stellar
            ? draw.stellarColorLinear.x
            : draw.appearance.albedoLinear.x),
        bits(draw.stellar
            ? draw.stellarColorLinear.y
            : draw.appearance.albedoLinear.y),
        bits(draw.stellar
            ? draw.stellarColorLinear.z
            : draw.appearance.albedoLinear.z),
        bits(draw.stellar
            ? std::clamp(
                  draw.stellarLimbDarkening,
                  0.0F,
                  1.0F)
            : draw.appearance.roughness),

        bits(draw.appearance.oceanFraction),
        bits(draw.appearance.iceFraction),
        bits(draw.stellar ? 1.0F : 0.0F),
        bits(
            draw.representation ==
                    celestial_representation::Representation::SmoothGlobe
                ? 3.0F
                : draw.representation ==
                          celestial_representation::Representation::
                              CachedDiscImpostor
                      ? 5.0F
                      : draw.representation ==
                                celestial_representation::Representation::
                                    AnalyticDiscImpostor
                            ? 4.0F
                            : 7.0F),

        bits(draw.appearance.emissionLinear.x),
        bits(draw.appearance.emissionLinear.y),
        bits(draw.appearance.emissionLinear.z),
        bits(std::clamp(
            draw.opacity,
            0.0F,
            1.0F)),

        bits(static_cast<f32>(mode)),
        bits(radiusNdc),
        1U,
        bits(std::max(
            draw.radiometricIntensity,
            0.0F)),

        bits(draw.lightDirectionBody.x),
        bits(draw.lightDirectionBody.y),
        bits(draw.lightDirectionBody.z),
        bits(std::max(
            draw.incidentLightScale,
            0.0F))
    };

    std::array<rhi::Texture*, 3> targets{
        &surfaceBaseRoughness,
        &surfaceNormalMetallic,
        &surfaceEmissionClass
    };

    commands.SetRenderTargets(
        targets,
        nullptr);
    commands.SetViewport({
        .x = 0.0F,
        .y = 0.0F,
        .width = static_cast<f32>(width),
        .height = static_cast<f32>(height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F
    });
    commands.SetScissor({
        .left = 0,
        .top = 0,
        .right = static_cast<i32>(width),
        .bottom = static_cast<i32>(height)
    });
    commands.SetGraphicsPipeline(
        *surfacePipeline_);
    commands.SetGraphicsConstants(
        constants);
    commands.Draw(6);
}
} // namespace orbit::celestial_far_render
