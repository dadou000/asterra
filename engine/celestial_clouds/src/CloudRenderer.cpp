#include <orbit/celestial_clouds/CloudRenderer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace orbit::celestial_clouds
{
namespace
{
constexpr const char* kVertexShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    const float2 positions[6] =
    {
        float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, -1.0),
        float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0)
    };
    const float2 uvs[6] =
    {
        float2(0.0, 1.0), float2(0.0, 0.0), float2(1.0, 1.0),
        float2(1.0, 1.0), float2(0.0, 0.0), float2(1.0, 0.0)
    };
    VSOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}
)";

constexpr const char* kCloudCommon = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct Constants
{
    float4 cameraAspect;      // body-frame camera position (m), aspect
    float4 forwardTanHalfFov; // camera forward, tan(vfov/2)
    float4 upNear;            // camera up, near plane (m)
    float4 sunFar;            // unit sun direction, far plane (m)
    float4 shell;             // planet radius, base alt, top alt, face resolution
    float4 optics;            // albedo, anisotropy, irradiance scale, opacity scale
    float4 atmosphere;        // atmosphere bottom radius, top radius, detail scale, depth pixels per output pixel
};

[[vk::push_constant]] Constants g;

// GpuCloudTexel: coverage, opticalDepth, albedo, cloudType (16 B)
[[vk::binding(0, 0)]] ByteAddressBuffer g_clouds : register(t0);
// 32^3 tileable cellular noise: R base shape, G/B/A erosion octaves (16 B)
[[vk::binding(1, 0)]] ByteAddressBuffer g_noise : register(t1);
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] Texture2D g_depth;
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] SamplerState g_depthSampler;

// Marching budget. Steps are distance-adaptive (fine near the camera, coarse far
// away) but never fewer than the budget needs to cross the whole shell segment.
static const uint kMaxSteps = 112u;
static const float kBaseStep = 60.0;
static const float kAdaptiveStep = 0.006;
static const float kMaxStep = 12000.0;
static const float kEmptySkip = 4.0;
static const float kBaseVoxelMeters = 700.0;   // 22 km noise period
static const float kDetailVoxelMeters = 78.0;  // 2.5 km noise period at detailScale 14
static const float kDetailFadeNear = 50000.0;
static const float kDetailFadeFar = 140000.0;
static const uint kShadowSteps = 16u;
static const float kPi = 3.14159265358979;

float3 ViewRay(float2 uv)
{
    const float3 forward = normalize(g.forwardTanHalfFov.xyz);
    const float3 right = normalize(cross(forward, normalize(g.upNear.xyz)));
    const float3 up = normalize(cross(right, forward));
    const float tanHalf = max(g.forwardTanHalfFov.w, 0.001);
    const float aspect = max(g.cameraAspect.w, 0.001);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return normalize(forward + right * (ndc.x * aspect * tanHalf) + up * (ndc.y * tanHalf));
}

bool RaySphere(float3 origin, float3 direction, float radius, out float t0, out float t1)
{
    const float b = dot(origin, direction);
    const float3 perpendicular = origin - b * direction;
    const float h2 = radius * radius - dot(perpendicular, perpendicular);
    t0 = 0.0;
    t1 = -1.0;
    if (h2 < 0.0)
    {
        return false;
    }
    const float h = sqrt(h2);
    t0 = -b - h;
    t1 = -b + h;
    return true;
}

float ReverseZViewDepth(float depth)
{
    const float nearPlane = max(g.upNear.w, 1.0e-5);
    const float farPlane = max(g.sunFar.w, nearPlane + 1.0e-4);
    return nearPlane * farPlane / max(depth * (farPlane - nearPlane) + nearPlane, 1.0e-6);
}

// Mirrors CloudField.cpp DirectionToCube; returns face and (u, v) in [-1, 1].
void DirectionToCube(float3 d, out uint face, out float2 uv)
{
    const float3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z)
    {
        if (d.x >= 0.0) { face = 0u; uv = float2(-d.z, d.y) / a.x; }
        else { face = 1u; uv = float2(d.z, d.y) / a.x; }
    }
    else if (a.y >= a.x && a.y >= a.z)
    {
        if (d.y >= 0.0) { face = 2u; uv = float2(d.x, -d.z) / a.y; }
        else { face = 3u; uv = float2(d.x, d.z) / a.y; }
    }
    else
    {
        if (d.z >= 0.0) { face = 4u; uv = float2(d.x, d.y) / a.z; }
        else { face = 5u; uv = float2(-d.x, d.y) / a.z; }
    }
}

// (coverage, column optical depth, cloud type) of the layer in direction d,
// bilinear in a cube face: one 16-byte load per corner.
float3 ColumnData(float3 d)
{
    const uint resolution = (uint)g.shell.w;
    uint face;
    float2 uv;
    DirectionToCube(d, face, uv);
    const float last = float(resolution - 1u);
    const float2 f = clamp((uv * 0.5 + 0.5) * last, 0.0, last);
    const uint2 p0 = (uint2)floor(f);
    const uint2 p1 = min(p0 + 1u, resolution - 1u);
    const float2 t = f - float2(p0);
    const uint faceBase = face * resolution * resolution;
    const uint4 i = faceBase + uint4(p0.y * resolution + p0.x, p0.y * resolution + p1.x,
                                     p1.y * resolution + p0.x, p1.y * resolution + p1.x);
    const float4 a = asfloat(g_clouds.Load4(i.x * 16u));
    const float4 b = asfloat(g_clouds.Load4(i.y * 16u));
    const float4 c = asfloat(g_clouds.Load4(i.z * 16u));
    const float4 e = asfloat(g_clouds.Load4(i.w * 16u));
    const float4 m = lerp(lerp(a, b, t.x), lerp(c, e, t.x), t.y);
    return float3(m.x, m.y, m.w);
}

float4 NoiseVoxel(int3 c)
{
    const uint3 w = uint3(c) & 31u;
    return asfloat(g_noise.Load4(((w.z * 32u + w.y) * 32u + w.x) * 16u));
}

// Trilinear lookup of the tileable noise; p is in voxel units.
float4 SampleNoise(float3 p)
{
    const float3 fl = floor(p);
    const float3 t = p - fl;
    const int3 c = int3(fl);
    const float4 n000 = NoiseVoxel(c);
    const float4 n100 = NoiseVoxel(c + int3(1, 0, 0));
    const float4 n010 = NoiseVoxel(c + int3(0, 1, 0));
    const float4 n110 = NoiseVoxel(c + int3(1, 1, 0));
    const float4 n001 = NoiseVoxel(c + int3(0, 0, 1));
    const float4 n101 = NoiseVoxel(c + int3(1, 0, 1));
    const float4 n011 = NoiseVoxel(c + int3(0, 1, 1));
    const float4 n111 = NoiseVoxel(c + int3(1, 1, 1));
    return lerp(lerp(lerp(n000, n100, t.x), lerp(n010, n110, t.x), t.y),
                lerp(lerp(n001, n101, t.x), lerp(n011, n111, t.x), t.y), t.z);
}

float3 RotateNoiseSpace(float3 p)
{
    return float3(
        dot(p, float3(0.36, 0.48, -0.80)),
        dot(p, float3(-0.80, 0.60, 0.0)),
        dot(p, float3(0.48, 0.64, 0.60)));
}

float Hash13(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float SmoothRange(float a, float b, float x)
{
    const float t = saturate((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}

// Vertical density profile per cloud type; each integrates to one over the
// layer so the column optical depth is honoured. Stratus is thin and low,
// cumulus has a flat base and a rounded top, cumulonimbus fills the layer with
// a denser anvil near the top.
float TypeProfile(float cloudType, float h)
{
    const float wB = SmoothRange(0.65, 0.9, cloudType);
    const float wS = (1.0 - SmoothRange(0.15, 0.5, cloudType)) * (1.0 - wB);
    const float wC = 1.0 - wS - wB;

    const float hs = h / 0.35;
    const float hc = h / 0.7;
    const float stratus =
        hs < 1.0 ? SmoothRange(0.0, 0.2, hs) * (1.0 - SmoothRange(0.6, 1.0, hs)) / 0.245 : 0.0;
    const float cumulus =
        hc < 1.0 ? SmoothRange(0.0, 0.1, hc) * (1.0 - SmoothRange(0.55, 1.0, hc)) / 0.5075 : 0.0;
    const float tower =
        SmoothRange(0.0, 0.08, h) * (0.7 + 0.3 * SmoothRange(0.55, 0.8, h)) *
        (1.0 - SmoothRange(0.88, 1.0, h)) / 0.7095;
    return wS * stratus + wC * cumulus + wB * tower;
}

// Extinction (1/m) at a point. detailAmount adds the erosion octaves (camera
// rays only, and only near the camera); the sun march uses the base shape.
float Extinction(float3 column, float3 position, float radius, float detailAmount)
{
    const float thickness = max(g.shell.z - g.shell.y, 1.0);
    const float h = (radius - g.shell.x - g.shell.y) / thickness;
    if (h <= 0.0 || h >= 1.0 || column.y <= 1.0e-4)
    {
        return 0.0;
    }
    const float profile = TypeProfile(column.z, h);
    if (profile <= 0.0)
    {
        return 0.0;
    }

    const float coverage = saturate(column.x);
    // Two non-aligned scales (one rotated) so the repeating noise lattice never reads as a grid.
    const float3 rotated = RotateNoiseSpace(position);
    const float baseMix =
        0.6 * SampleNoise(position / kBaseVoxelMeters).r +
        0.4 * SampleNoise(rotated / (kBaseVoxelMeters * 2.6) + 5.0).r;
    const float baseValue = saturate(0.5 + (baseMix - 0.5) * 1.5);
    // Hard-edged billows: the density ramps up over a narrow band of the base noise.
    float shape = saturate(2.5 * (baseValue - (1.0 - coverage)) / max(coverage, 0.05));
    if (detailAmount > 0.0 && shape > 0.0)
    {
        const float detailMeters = kDetailVoxelMeters * 14.0 / max(g.atmosphere.z, 1.0);
        const float4 d = SampleNoise(rotated / detailMeters + 17.0);
        const float erosion = 0.5 * d.g + 0.3 * d.b + 0.2 * d.a;
        const float depth = 0.65 * (0.5 + 0.5 * saturate(column.z * 1.6)) * detailAmount;
        shape = saturate((shape - depth * (1.0 - erosion)) / max(1.0 - depth, 0.05));
    }
    return column.y / thickness * profile * shape * 1.7 * g.optics.w;
}

float DualHg(float c, float anisotropy)
{
    const float g1 = anisotropy;
    const float g2 = -0.3 * anisotropy;
    const float a = (1.0 - g1 * g1) / (4.0 * kPi * pow(max(1.0 + g1 * g1 - 2.0 * g1 * c, 1.0e-4), 1.5));
    const float b = (1.0 - g2 * g2) / (4.0 * kPi * pow(max(1.0 + g2 * g2 - 2.0 * g2 * c, 1.0e-4), 1.5));
    return lerp(b, a, 0.7);
}

float2 LutUv(float radius, float sunMu)
{
    const float rb = g.atmosphere.x;
    const float rt = g.atmosphere.y;
    const float unit = saturate((radius * radius - rb * rb) / max(rt * rt - rb * rb, 1.0));
    return float2(saturate(sunMu * 0.5 + 0.5), unit);
}

// Optical depth towards the sun: four increasing steps over 1.2 km on the base
// shape, plus a coarse tail for the rest of the way out of the layer.
float SunOpticalDepth(float3 position, float3 sun, float outer)
{
    float sunT0;
    float sunT1;
    if (!RaySphere(position, sun, outer, sunT0, sunT1) || sunT1 <= 0.0)
    {
        return 0.0;
    }
    const float marches[4] = { 100.0, 220.0, 380.0, 500.0 };
    float depth = 0.0;
    float travelled = 0.0;
    float lastSigma = 0.0;
    [unroll]
    for (uint i = 0u; i < 4u; ++i)
    {
        const float stepLength = min(marches[i], max(sunT1 - travelled, 0.0));
        const float3 samplePosition = position + sun * (travelled + 0.5 * stepLength);
        const float radius = length(samplePosition);
        lastSigma = Extinction(ColumnData(samplePosition / max(radius, 1.0)), samplePosition, radius, 0.0);
        depth += lastSigma * stepLength;
        travelled += stepLength;
    }
    const float tail = clamp(sunT1 - travelled, 0.0, 4000.0);
    return depth + lastSigma * tail * 0.5;
}

)";

constexpr const char* kMarchBindings = R"([[vk::binding(3, 0)]] [[vk::combinedImageSampler]] Texture2D g_transmittance;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] SamplerState g_transmittanceSampler;
)";

constexpr const char* kMarchMain = R"(float4 main(VSOutput input) : SV_Target0
{
    // Output: premultiplied in-scattered radiance (rgb) and transmittance (a),
    // at reduced resolution; the composite pass applies it over the scene.
    const float4 empty = float4(0.0, 0.0, 0.0, 1.0);
    const int2 pixel = int2(input.position.xy);

    const float3 origin = g.cameraAspect.xyz;
    const float3 direction = ViewRay(input.uv);
    const float inner = g.shell.x + g.shell.y;
    const float outer = g.shell.x + g.shell.z;

    float o0, o1;
    if (!RaySphere(origin, direction, outer, o0, o1) || o1 <= 0.0)
    {
        return empty;
    }

    float tStart = max(o0, 0.0);
    float tEnd = o1;
    float i0, i1;
    if (RaySphere(origin, direction, inner, i0, i1) && i1 > 0.0)
    {
        if (i0 > 0.0)
        {
            tEnd = i0;                // camera outside the shell, ray enters the base
        }
        else
        {
            tStart = max(i1, tStart); // camera under the base looking up
        }
    }

    // Nearest (reverse-Z: largest) depth of the full-resolution pixels this output pixel covers.
    uint depthWidth, depthHeight;
    g_depth.GetDimensions(depthWidth, depthHeight);
    const int2 depthBase = int2(float2(pixel) * g.atmosphere.w);
    const int2 depthMax = int2(depthWidth - 1u, depthHeight - 1u);
    const float depth = max(
        max(g_depth.Load(int3(min(depthBase, depthMax), 0)).r,
            g_depth.Load(int3(min(depthBase + int2(1, 0), depthMax), 0)).r),
        max(g_depth.Load(int3(min(depthBase + int2(0, 1), depthMax), 0)).r,
            g_depth.Load(int3(min(depthBase + int2(1, 1), depthMax), 0)).r));
    if (depth > 0.0)
    {
        const float surfaceT =
            ReverseZViewDepth(depth) / max(dot(direction, normalize(g.forwardTanHalfFov.xyz)), 1.0e-5);
        tEnd = min(tEnd, surfaceT);
    }

    if (tEnd <= tStart)
    {
        return empty;
    }

    const float3 sun = normalize(g.sunFar.xyz);
    const float cosine = dot(direction, sun);
    const float anisotropy = g.optics.y;
    const float albedo = g.optics.x;
    const float irradiance = max(g.optics.z, 0.0);

    // Sun colour through the atmosphere once per pixel, at the segment midpoint.
    const float3 midPoint = origin + direction * (0.5 * (tStart + tEnd));
    const float midRadius = length(midPoint);
    const float3 atmosphereSun =
        g_transmittance.SampleLevel(
            g_transmittanceSampler, LutUv(midRadius, dot(midPoint / max(midRadius, 1.0), sun)), 0).rgb;

    // Three scattering octaves: energy, extinction and anisotropy halve each time.
    const float phase0 = DualHg(cosine, anisotropy);
    const float phase1 = DualHg(cosine, anisotropy * 0.5);
    const float phase2 = DualHg(cosine, anisotropy * 0.25);

    // Steps grow geometrically from the base step; the growth rate is solved per
    // pixel so the whole segment is covered in kMaxSteps steps. A long grazing ray
    // therefore keeps fine steps near the camera and only coarsens with distance
    // (a uniform budget step would blur every near cloud on the horizon).
    const float segment = tEnd - tStart;
    float growthLow = kAdaptiveStep;
    float growthHigh = 0.25;
    [unroll]
    for (uint s = 0u; s < 10u; ++s)
    {
        const float mid = 0.5 * (growthLow + growthHigh);
        const float covered = kBaseStep / mid * (pow(1.0 + mid, float(kMaxSteps)) - 1.0);
        if (covered > segment) { growthHigh = mid; } else { growthLow = mid; }
    }
    const float growth = growthHigh;

    // Stratified jitter: each sample sits at a random offset inside its own step.
    const float jitter = Hash13(float3(pixel, 7.0));
    float cursor = tStart;
    float transmittance = 1.0;
    float3 radiance = 0.0;

    [loop]
    for (uint i = 0u; i < kMaxSteps && cursor < tEnd && transmittance > 0.02; ++i)
    {
        const float stepLength = min(max(kBaseStep, (cursor - tStart) * growth), kMaxStep);
        const float t = cursor + jitter * stepLength;
        cursor += stepLength;
        if (t >= tEnd)
        {
            break;
        }
        const float3 position = origin + direction * t;
        const float radius = length(position);

        const float3 column = ColumnData(position / max(radius, 1.0));
        const float detailAmount = 1.0 - SmoothRange(kDetailFadeNear, kDetailFadeFar, t);
        const float sigma = Extinction(column, position, radius, detailAmount);
        if (sigma <= 0.0)
        {
            // Nothing here: leap ahead instead of marching empty space finely.
            // Capped so a coarse far step can never leap over a whole cloud.
            cursor += column.y <= 1.0e-4 ? min(stepLength * (kEmptySkip - 1.0), 4000.0) : 0.0;
            continue;
        }

        const float sunDepth = SunOpticalDepth(position, sun, outer);
        const float h = saturate((radius - g.shell.x - g.shell.y) / max(g.shell.z - g.shell.y, 1.0));
        const float skyFacing = saturate(dot(position / max(radius, 1.0), sun) * 0.5 + 0.5);
        const float multi =
            phase0 * exp(-sunDepth) +
            0.5 * phase1 * exp(-sunDepth * 0.5) +
            0.25 * phase2 * exp(-sunDepth * 0.25) +
            0.3 * exp(-sunDepth * 0.15);
        const float ambient = (0.02 + 0.06 * h) * skyFacing;
        const float3 source = irradiance * albedo * atmosphereSun * (multi + ambient);

        const float stepTransmittance = exp(-sigma * stepLength);
        radiance += transmittance * source * (1.0 - stepTransmittance);
        transmittance *= stepTransmittance;
    }

    return float4(radiance, transmittance);
}
)";

constexpr const char* kShadowMain = R"(float4 main(VSOutput input) : SV_Target0
{
    // Sun transmittance through the cloud shell at the surface seen by this
    // pixel, from the same density field the camera march draws.
    const float4 lit = float4(1.0, 1.0, 1.0, 1.0);
    const int2 pixel = int2(input.position.xy);
    uint depthWidth, depthHeight;
    g_depth.GetDimensions(depthWidth, depthHeight);
    const int2 depthBase = int2(float2(pixel) * g.atmosphere.w);
    const int2 depthMax = int2(depthWidth - 1u, depthHeight - 1u);
    const float depth = max(
        max(g_depth.Load(int3(min(depthBase, depthMax), 0)).r,
            g_depth.Load(int3(min(depthBase + int2(1, 0), depthMax), 0)).r),
        max(g_depth.Load(int3(min(depthBase + int2(0, 1), depthMax), 0)).r,
            g_depth.Load(int3(min(depthBase + int2(1, 1), depthMax), 0)).r));
    if (depth <= 0.0)
    {
        return lit;
    }

    const float3 origin = g.cameraAspect.xyz;
    const float3 direction = ViewRay(input.uv);
    const float surfaceT =
        ReverseZViewDepth(depth) / max(dot(direction, normalize(g.forwardTanHalfFov.xyz)), 1.0e-5);
    const float3 surface = origin + direction * surfaceT;
    const float3 sun = normalize(g.sunFar.xyz);
    const float radius = length(surface);
    const float inner = g.shell.x + g.shell.y;
    const float outer = g.shell.x + g.shell.z;
    if (radius >= outer || dot(surface / max(radius, 1.0), sun) <= 0.0)
    {
        return lit;
    }

    float o0, o1;
    if (!RaySphere(surface, sun, outer, o0, o1) || o1 <= 0.0)
    {
        return lit;
    }
    float segmentStart = 0.0;
    if (radius < inner)
    {
        float i0, i1;
        if (RaySphere(surface, sun, inner, i0, i1) && i1 > 0.0)
        {
            segmentStart = i1;
        }
    }
    const float segmentEnd = min(o1, segmentStart + 60000.0);
    if (segmentEnd <= segmentStart)
    {
        return lit;
    }

    const float stepLength = (segmentEnd - segmentStart) / float(kShadowSteps);
    const float jitter = Hash13(float3(pixel, 3.0));
    float opticalDepth = 0.0;
    [loop]
    for (uint i = 0u; i < kShadowSteps; ++i)
    {
        const float3 samplePosition = surface + sun * (segmentStart + (float(i) + jitter) * stepLength);
        const float sampleRadius = length(samplePosition);
        opticalDepth +=
            Extinction(ColumnData(samplePosition / max(sampleRadius, 1.0)), samplePosition, sampleRadius, 0.5) *
            stepLength;
    }
    return float4(exp(-opticalDepth), 0.0, 0.0, 1.0);
}
)";

[[nodiscard]] std::string MarchSource()
{
    return std::string(kCloudCommon) + kMarchBindings + kMarchMain;
}

[[nodiscard]] std::string ShadowSource()
{
    return std::string(kCloudCommon) + kShadowMain;
}


constexpr const char* kCompositePixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]] [[vk::combinedImageSampler]] Texture2D g_clouds;
[[vk::binding(0, 0)]] [[vk::combinedImageSampler]] SamplerState g_cloudsSampler;

// Alpha blend turns (L / (1 - T), 1 - T) into L + T * scene.
float4 main(VSOutput input) : SV_Target0
{
    const float4 c = g_clouds.SampleLevel(g_cloudsSampler, input.uv, 0);
    const float alpha = saturate(1.0 - c.a);
    if (alpha <= 1.0e-4)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return float4(c.rgb / alpha, alpha);
}
)";

constexpr u32 kNoiseGrid = 32U;

[[nodiscard]] u32 HashCell(u32 x, u32 y, u32 z, const u32 salt) noexcept
{
    u32 h = x * 0x8DA6B343U ^ y * 0xD8163841U ^ z * 0xCB1AB31FU ^ salt * 0x165667B1U;
    h ^= h >> 15U;
    h *= 0x2C1B3C6DU;
    h ^= h >> 12U;
    h *= 0x297A2D39U;
    h ^= h >> 15U;
    return h;
}

// Tileable cellular (worley) noise: distance to the nearest feature point of a
// jittered grid with 'cells' cells per axis, wrapped so the volume repeats.
[[nodiscard]] f32 Worley(
    const u32 x,
    const u32 y,
    const u32 z,
    const u32 cells,
    const u32 salt) noexcept
{
    const f32 scale = static_cast<f32>(cells) / static_cast<f32>(kNoiseGrid);
    const f32 px = (static_cast<f32>(x) + 0.5F) * scale;
    const f32 py = (static_cast<f32>(y) + 0.5F) * scale;
    const f32 pz = (static_cast<f32>(z) + 0.5F) * scale;
    const i32 cx = static_cast<i32>(std::floor(px));
    const i32 cy = static_cast<i32>(std::floor(py));
    const i32 cz = static_cast<i32>(std::floor(pz));
    const i32 n = static_cast<i32>(cells);

    f32 best = 4.0F;
    for (i32 dz = -1; dz <= 1; ++dz)
    {
        for (i32 dy = -1; dy <= 1; ++dy)
        {
            for (i32 dx = -1; dx <= 1; ++dx)
            {
                const i32 gx = cx + dx;
                const i32 gy = cy + dy;
                const i32 gz = cz + dz;
                const u32 h = HashCell(
                    static_cast<u32>(((gx % n) + n) % n),
                    static_cast<u32>(((gy % n) + n) % n),
                    static_cast<u32>(((gz % n) + n) % n),
                    salt);
                const f32 fx = static_cast<f32>(gx) + static_cast<f32>(h & 0xFFU) / 255.0F;
                const f32 fy = static_cast<f32>(gy) + static_cast<f32>((h >> 8U) & 0xFFU) / 255.0F;
                const f32 fz = static_cast<f32>(gz) + static_cast<f32>((h >> 16U) & 0xFFU) / 255.0F;
                const f32 ex = fx - px;
                const f32 ey = fy - py;
                const f32 ez = fz - pz;
                best = std::min(best, ex * ex + ey * ey + ez * ez);
            }
        }
    }
    return std::sqrt(best);
}

// R: base shape (fbm of inverted worley), G/B/A: erosion octaves, coarse to fine.
[[nodiscard]] std::vector<f32> BakeCloudNoise()
{
    const std::size_t voxels =
        static_cast<std::size_t>(kNoiseGrid) * kNoiseGrid * kNoiseGrid;
    std::vector<f32> data(voxels * 4U);
    std::array<f32, 4> low{1.0e9F, 1.0e9F, 1.0e9F, 1.0e9F};
    std::array<f32, 4> high{-1.0e9F, -1.0e9F, -1.0e9F, -1.0e9F};

    for (u32 z = 0U; z < kNoiseGrid; ++z)
    {
        for (u32 y = 0U; y < kNoiseGrid; ++y)
        {
            for (u32 x = 0U; x < kNoiseGrid; ++x)
            {
                const f32 w3 = 1.0F - std::min(Worley(x, y, z, 3U, 1U), 1.0F);
                const f32 w6 = 1.0F - std::min(Worley(x, y, z, 6U, 2U), 1.0F);
                const f32 w12 = 1.0F - std::min(Worley(x, y, z, 12U, 3U), 1.0F);
                const std::size_t index =
                    ((static_cast<std::size_t>(z) * kNoiseGrid + y) * kNoiseGrid + x) * 4U;
                data[index + 0U] = 0.6F * w3 + 0.3F * w6 + 0.1F * w12;
                data[index + 1U] = w3;
                data[index + 2U] = w6;
                data[index + 3U] = w12;
                for (std::size_t c = 0U; c < 4U; ++c)
                {
                    low[c] = std::min(low[c], data[index + c]);
                    high[c] = std::max(high[c], data[index + c]);
                }
            }
        }
    }

    // Stretch each channel to [0, 1] so the shader's thresholds are meaningful.
    for (std::size_t v = 0U; v < voxels; ++v)
    {
        for (std::size_t c = 0U; c < 4U; ++c)
        {
            const f32 range = std::max(high[c] - low[c], 1.0e-6F);
            data[v * 4U + c] = (data[v * 4U + c] - low[c]) / range;
        }
    }
    return data;
}

[[nodiscard]] u32 Bits(const f32 value) noexcept
{
    return std::bit_cast<u32>(value);
}
} // namespace

CloudRenderer::CloudRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto vertex = compiler.Compile({
        .source = kVertexShader,
        .entryPoint = "main",
        .stage = shader::Stage::Vertex,
        .debug = false});
    const std::string marchSource = MarchSource();
    const auto pixel = compiler.Compile({
        .source = marchSource,
        .entryPoint = "main",
        .stage = shader::Stage::Pixel,
        .debug = false});

    pipeline_ = device.CreateGraphicsPipeline({
        .vertexShader = {.data = vertex.bytecode.data(), .size = vertex.bytecode.size()},
        .pixelShader = {.data = pixel.bytecode.data(), .size = pixel.bytecode.size()},
        .vertexAttributes = {},
        .vertexStrideBytes = 0U,
        .pushConstantDwords = 28U,
        .shaderResourceBuffers = 2U,
        .sampledTextures = 2U,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid,
        .cullMode = rhi::CullMode::None,
        .blendMode = rhi::BlendMode::Opaque,
        .depthTest = false,
        .depthWrite = false,
        .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float},
        .colorAttachmentCount = 1U});

    const auto compositePixel = compiler.Compile({
        .source = kCompositePixelShader,
        .entryPoint = "main",
        .stage = shader::Stage::Pixel,
        .debug = false});
    compositePipeline_ = device.CreateGraphicsPipeline({
        .vertexShader = {.data = vertex.bytecode.data(), .size = vertex.bytecode.size()},
        .pixelShader = {.data = compositePixel.bytecode.data(), .size = compositePixel.bytecode.size()},
        .vertexAttributes = {},
        .vertexStrideBytes = 0U,
        .pushConstantDwords = 0U,
        .shaderResourceBuffers = 0U,
        .sampledTextures = 1U,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid,
        .cullMode = rhi::CullMode::None,
        .blendMode = rhi::BlendMode::Alpha,
        .depthTest = false,
        .depthWrite = false,
        .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float},
        .colorAttachmentCount = 1U});

    const std::string shadowSource = ShadowSource();
    const auto shadowPixel = compiler.Compile({
        .source = shadowSource,
        .entryPoint = "main",
        .stage = shader::Stage::Pixel,
        .debug = false});
    shadowPipeline_ = device.CreateGraphicsPipeline({
        .vertexShader = {.data = vertex.bytecode.data(), .size = vertex.bytecode.size()},
        .pixelShader = {.data = shadowPixel.bytecode.data(), .size = shadowPixel.bytecode.size()},
        .vertexAttributes = {},
        .vertexStrideBytes = 0U,
        .pushConstantDwords = 28U,
        .shaderResourceBuffers = 2U,
        .sampledTextures = 1U,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid,
        .cullMode = rhi::CullMode::None,
        .blendMode = rhi::BlendMode::Opaque,
        .depthTest = false,
        .depthWrite = false,
        .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float},
        .colorAttachmentCount = 1U});

    const auto noise = BakeCloudNoise();
    noise_ = device.CreateBuffer({
        .sizeBytes = static_cast<u64>(noise.size() * sizeof(f32)),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource});
    if (!noise_)
    {
        throw std::runtime_error("Failed to allocate cloud noise volume.");
    }
    std::memcpy(noise_->Map(), noise.data(), noise.size() * sizeof(f32));
    noise_->Unmap();
}

CloudRenderer::~CloudRenderer() = default;

void CloudRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& depth,
    celestial_atmosphere::GpuAtmosphereLuts& luts,
    GpuCloudFieldProduct& field,
    rhi::Texture& target,
    const u32 width,  // size of the (reduced resolution) cloud target
    const u32 height,
    const f64 referenceRadiusMeters,
    const CloudLayerParameters& layer,
    const celestial_atmosphere::AtmosphereParameters& atmosphere,
    const celestial_atmosphere::AtmosphereRenderView& view)
{
    if (width == 0U || height == 0U || field.LayerCount() == 0U ||
        field.FaceResolution() < 2U)
    {
        return;
    }

    const auto f = [](const f64 value) { return Bits(static_cast<f32>(value)); };

    const std::array<u32, 28> constants{
        f(view.cameraPositionMeters.x),
        f(view.cameraPositionMeters.y),
        f(view.cameraPositionMeters.z),
        Bits(static_cast<f32>(width) / static_cast<f32>(height)),

        Bits(view.forward.x), Bits(view.forward.y), Bits(view.forward.z),
        Bits(std::tan(view.verticalFovRadians * 0.5F)),

        Bits(view.up.x), Bits(view.up.y), Bits(view.up.z),
        Bits(std::max(view.nearPlaneMeters, 1.0e-5F)),

        Bits(view.sunDirection.x), Bits(view.sunDirection.y), Bits(view.sunDirection.z),
        Bits(std::max(view.farPlaneMeters, view.nearPlaneMeters + 1.0e-4F)),

        f(referenceRadiusMeters),
        f(layer.baseAltitudeMeters),
        f(layer.topAltitudeMeters),
        Bits(static_cast<f32>(field.FaceResolution())),

        f(std::clamp(layer.singleScatteringAlbedo, 0.0, 1.0)),
        f(std::clamp(layer.anisotropy, -0.95, 0.95)),
        Bits(std::max(view.irradianceScale, 0.0F)),
        f(0.6),

        f(atmosphere.bottomRadiusMeters),
        f(atmosphere.topRadiusMeters),
        f(layer.detailScale),
        Bits(static_cast<f32>(depth.Width()) / static_cast<f32>(width))};

    commands.SetRenderTarget(target);
    commands.SetViewport({
        .x = 0.0F, .y = 0.0F,
        .width = static_cast<f32>(width), .height = static_cast<f32>(height),
        .minDepth = 0.0F, .maxDepth = 1.0F});
    commands.SetScissor({
        .left = 0, .top = 0,
        .right = static_cast<i32>(width), .bottom = static_cast<i32>(height)});

    commands.SetGraphicsPipeline(*pipeline_);
    commands.SetGraphicsConstants(constants);
    commands.SetGraphicsBuffer(0, field.Buffer());
    commands.SetGraphicsBuffer(1, *noise_);
    commands.SetGraphicsTexture(0, depth);
    commands.SetGraphicsTexture(1, luts.Transmittance());
    commands.Draw(6);
}

void CloudRenderer::DrawShadow(
    rhi::CommandList& commands,
    rhi::Texture& depth,
    GpuCloudFieldProduct& field,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    const f64 referenceRadiusMeters,
    const CloudLayerParameters& layer,
    const celestial_atmosphere::AtmosphereRenderView& view)
{
    if (width == 0U || height == 0U || field.LayerCount() == 0U ||
        field.FaceResolution() < 2U)
    {
        return;
    }

    const auto f = [](const f64 value) { return Bits(static_cast<f32>(value)); };

    const std::array<u32, 28> constants{
        f(view.cameraPositionMeters.x),
        f(view.cameraPositionMeters.y),
        f(view.cameraPositionMeters.z),
        Bits(static_cast<f32>(width) / static_cast<f32>(height)),

        Bits(view.forward.x), Bits(view.forward.y), Bits(view.forward.z),
        Bits(std::tan(view.verticalFovRadians * 0.5F)),

        Bits(view.up.x), Bits(view.up.y), Bits(view.up.z),
        Bits(std::max(view.nearPlaneMeters, 1.0e-5F)),

        Bits(view.sunDirection.x), Bits(view.sunDirection.y), Bits(view.sunDirection.z),
        Bits(std::max(view.farPlaneMeters, view.nearPlaneMeters + 1.0e-4F)),

        f(referenceRadiusMeters),
        f(layer.baseAltitudeMeters),
        f(layer.topAltitudeMeters),
        Bits(static_cast<f32>(field.FaceResolution())),

        0U, 0U, 0U, f(0.6),

        0U, 0U,
        f(layer.detailScale),
        Bits(static_cast<f32>(depth.Width()) / static_cast<f32>(width))};

    commands.SetRenderTarget(target);
    commands.SetViewport({
        .x = 0.0F, .y = 0.0F,
        .width = static_cast<f32>(width), .height = static_cast<f32>(height),
        .minDepth = 0.0F, .maxDepth = 1.0F});
    commands.SetScissor({
        .left = 0, .top = 0,
        .right = static_cast<i32>(width), .bottom = static_cast<i32>(height)});

    commands.SetGraphicsPipeline(*shadowPipeline_);
    commands.SetGraphicsConstants(constants);
    commands.SetGraphicsBuffer(0, field.Buffer());
    commands.SetGraphicsBuffer(1, *noise_);
    commands.SetGraphicsTexture(0, depth);
    commands.Draw(6);
}

void CloudRenderer::Composite(
    rhi::CommandList& commands,
    rhi::Texture& clouds,
    rhi::Texture& target,
    const u32 width,
    const u32 height)
{
    if (width == 0U || height == 0U)
    {
        return;
    }

    commands.SetRenderTarget(target);
    commands.SetViewport({
        .x = 0.0F, .y = 0.0F,
        .width = static_cast<f32>(width), .height = static_cast<f32>(height),
        .minDepth = 0.0F, .maxDepth = 1.0F});
    commands.SetScissor({
        .left = 0, .top = 0,
        .right = static_cast<i32>(width), .bottom = static_cast<i32>(height)});
    commands.SetGraphicsPipeline(*compositePipeline_);
    commands.SetGraphicsTexture(0, clouds);
    commands.Draw(6);
}
} // namespace orbit::celestial_clouds
