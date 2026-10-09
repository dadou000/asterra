#include <orbit/lighting/ScreenSpaceFinalGather.hpp>
#include <orbit/lighting/SdfTraceShader.hpp>

#include <algorithm>
#include <cstdlib>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace orbit::lighting
{
namespace
{
constexpr const char* kGatherCs = R"(
[[vk::binding(0, 0)]]
StructuredBuffer<uint4> g_particleLightGrid : register(t0);

// Merged mesh distance field (world-space GI fallback); dummies when absent.
// Corner-packed distances (see SdfTraceShader.hpp, SDF_DIST_CORNERS).
#define SDF_DIST_CORNERS 1
[[vk::binding(1, 0)]] RWStructuredBuffer<uint4> g_sdfDist : register(u20);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> g_sdfAlbedo : register(u21);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> g_sdfNormal : register(u22);
[[vk::binding(4, 0)]] RWStructuredBuffer<float4> g_sdfRadiance : register(u23);
// Per-frame probe counters (surface pixels, smooth pixels, low-coverage pixels) the CPU
// reads a few frames later to skip passes whose inputs are absent.
[[vk::binding(5, 0)]] RWByteAddressBuffer g_needStats : register(u24);

[[vk::binding(6, 0)]]
RWTexture2D<float4> g_currentIndirect : register(u1);
[[vk::binding(7, 0)]]
RWTexture2D<float4> g_currentMeta : register(u2);

[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_sceneColor : register(t3);
[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sceneSampler : register(s3);

[[vk::binding(9, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_baseRoughness : register(t4);
[[vk::binding(9, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_baseSampler : register(s4);

[[vk::binding(10, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_normalMetallic : register(t5);
[[vk::binding(10, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_normalSampler : register(s5);

[[vk::binding(11, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_emissionClass : register(t6);
[[vk::binding(11, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_emissionSampler : register(s6);

[[vk::binding(12, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth : register(t7);
[[vk::binding(12, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler : register(s7);

[[vk::binding(13, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_previousIndirect : register(t8);
[[vk::binding(13, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_previousIndirectSampler : register(s8);

[[vk::binding(14, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_previousMeta : register(t9);
[[vk::binding(14, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_previousMetaSampler : register(s9);

struct Constants
{
    uint width;
    uint height;
    uint stepsPerRay;
    uint historyCompatible;

    float4 forwardAspect;
    float4 upTanHalfFov;
    float4 depthRangeRadius;
    float4 gatherTuning;
    float4 cameraFrameParticleGrid;
    float4 sdfOriginVoxel;       // volume voxel (0,0,0) centre relative to the camera, voxel size
    float4 sdfDimensionsEnable;  // dimensions, enable (1 = a field is bound)
};

[[vk::push_constant]]
Constants g;

#define SDF_ORIGIN g.sdfOriginVoxel.xyz
#define SDF_VOXEL g.sdfOriginVoxel.w
#define SDF_DIMS int3(g.sdfDimensionsEnable.xyz)
//SDF_TRACE_INCLUDE

static const float kSdfGatherDistance = 14.0;

float ReverseZViewDepth(float depth)
{
    const float nearPlane =
        max(g.depthRangeRadius.x, 1.0e-5);
    const float farPlane =
        max(g.depthRangeRadius.y, nearPlane + 1.0e-4);

    return
        nearPlane * farPlane /
        max(
            depth * (farPlane - nearPlane) +
                nearPlane,
            1.0e-6);
}

void CameraBasis(
    out float3 forward,
    out float3 right,
    out float3 up,
    out float aspect,
    out float tanHalfFov)
{
    forward = normalize(g.forwardAspect.xyz);
    const float3 requestedUp =
        normalize(g.upTanHalfFov.xyz);
    right =
        normalize(cross(forward, requestedUp));
    up =
        normalize(cross(right, forward));
    aspect =
        max(g.forwardAspect.w, 0.001);
    tanHalfFov =
        max(g.upTanHalfFov.w, 0.001);
}

float3 ViewRay(float2 uv)
{
    float3 forward;
    float3 right;
    float3 up;
    float aspect;
    float tanHalfFov;
    CameraBasis(
        forward,
        right,
        up,
        aspect,
        tanHalfFov);

    const float2 ndc = {
        uv.x * 2.0 - 1.0,
        1.0 - uv.y * 2.0
    };

    return
        normalize(
            forward +
            right *
                (ndc.x * aspect * tanHalfFov) +
            up *
                (ndc.y * tanHalfFov));
}

float3 ReconstructPosition(
    float2 uv,
    float depth)
{
    const float3 ray =
        ViewRay(uv);
    const float3 forward =
        normalize(g.forwardAspect.xyz);
    const float viewDepth =
        ReverseZViewDepth(depth);
    const float forwardProjection =
        max(dot(ray, forward), 1.0e-5);

    return
        ray *
        (viewDepth / forwardProjection);
}

bool ProjectPoint(
    float3 position,
    out float2 uv,
    out float viewDepth)
{
    float3 forward;
    float3 right;
    float3 up;
    float aspect;
    float tanHalfFov;

    CameraBasis(
        forward,
        right,
        up,
        aspect,
        tanHalfFov);

    viewDepth =
        dot(position, forward);

    if (viewDepth <=
        max(g.depthRangeRadius.x, 1.0e-5))
    {
        uv = 0.0;
        return false;
    }

    const float2 ndc = {
        dot(position, right) /
            (viewDepth * tanHalfFov * aspect),
        dot(position, up) /
            (viewDepth * tanHalfFov)
    };

    uv = float2(
        ndc.x * 0.5 + 0.5,
        0.5 - ndc.y * 0.5
    );

    return
        all(uv >= 0.0) &&
        all(uv <= 1.0);
}


float4 SampleParticleLightGrid(float3 framePosition)
{
    if (g.cameraFrameParticleGrid.w <= 0.0) return 0.0;
    const uint4 meta = g_particleLightGrid[1];
    const float3 origin = float3(asfloat(meta.x),asfloat(meta.y),asfloat(meta.z));
    const float cellSize = asfloat(meta.w);
    if (!(cellSize > 0.0)) return 0.0;
    const int3 cell = int3(floor((framePosition-origin)/cellSize));
    if (any(cell < 0) || any(cell >= int3(32,32,32))) return 0.0;
    const uint index = 2u + (uint(cell.z)*32u + uint(cell.y))*32u + uint(cell.x);
    const uint4 packed = g_particleLightGrid[index];
    return float4(float(packed.x)/4096.0,float3(packed.yzw)/1024.0);
}

float ParticleGridTransmittance(float3 start,float3 direction,float distance)
{
    if (g.cameraFrameParticleGrid.w <= 0.0 || distance <= 1.0e-4) return 1.0;
    const float3 ray = normalize(direction);
    const float boundedDistance = min(distance,192.0);
    const float stepLength = max(boundedDistance/6.0,8.0);
    float optical = 0.0;
    [unroll] for(uint i=1u;i<=6u;++i)
    {
        const float t=min(stepLength*float(i),boundedDistance);
        optical += SampleParticleLightGrid(start+ray*t).x*0.18;
    }
    return exp(-min(optical,20.0));
}

float3 ParticleGridEmissionAlong(float3 start,float3 direction,float distance)
{
    if (g.cameraFrameParticleGrid.w <= 0.0 || distance <= 1.0e-4) return 0.0;
    const float3 ray=normalize(direction);
    const float boundedDistance=min(distance,192.0);
    float3 sum=0.0;
    [unroll] for(uint i=1u;i<=4u;++i)
    {
        const float t=boundedDistance*(float(i)-0.5)/4.0;
        sum += SampleParticleLightGrid(start+ray*t).yzw;
    }
    return sum*0.25;
}

float Hash12(float2 p)
{
    const float3 p3 =
        frac(float3(p.xyx) * 0.1031);
    const float3 q =
        p3 + dot(p3, p3.yzx + 33.33);
    return
        frac((q.x + q.y) * q.z);
}

float3 TangentDirection(
    float3 normal,
    float angle,
    float elevation)
{
    const float3 helper =
        abs(normal.y) < 0.95
            ? float3(0.0, 1.0, 0.0)
            : float3(1.0, 0.0, 0.0);

    const float3 tangent =
        normalize(cross(helper, normal));
    const float3 bitangent =
        normalize(cross(normal, tangent));

    const float radial =
        sqrt(
            max(
                1.0 -
                elevation * elevation,
                0.0));

    return normalize(
        tangent * cos(angle) * radial +
        bitangent * sin(angle) * radial +
        normal * elevation);
}

void TraceRay(
    uint2 pixel,
    float3 normal,
    float3 surfacePosition,
    float3 surfaceFramePosition,
    float radius,
    float thickness,
    uint steps,
    uint rayIndex,
    inout float3 accumulated,
    inout float accumulatedWeight,
    inout float validRayCount)
{
    // While the camera is still the sample pattern changes every frame so the
    // temporal blend accumulates different directions; in motion it stays fixed
    // (stable noise, no shimmer).
    const float frameJitter =
        (g.historyCompatible & 1u) != 0u
            ? float((g.historyCompatible >> 8u) & 255u) * 0.61803398875
            : 0.0;
    const float randomRotation =
        (Hash12(float2(pixel)) + frac(frameJitter)) *
        6.28318530718;
    {
        const float angle =
            randomRotation +
            (float(rayIndex) + 0.5) *
                1.57079632679;

)"
R"(        const float elevation =
            lerp(
                0.28,
                0.72,
                frac(
                    float(rayIndex) *
                        0.61803398875 +
                    Hash12(
                        float2(pixel) +
                        float2(17.0, 41.0) +
                        frameJitter * 3.7)));

        const float3 direction =
            TangentDirection(
                normal,
                angle,
                elevation);

        float previousDelta =
            -1.0e20;
        float previousT = 0.0;
        bool resolved = false;

        // A per-pixel, per-ray offset of the step positions turns the
        // fixed step spacing (which quantised the hit distance and showed up
        // as concentric bands around bright emitters) into fine noise that
        // the temporal accumulation resolves.
        const float stepJitter =
            Hash12(
                float2(pixel) +
                float2(53.0, 7.0) * (float(rayIndex) + 1.0) +
                frameJitter * 5.3);

        [loop]
        for (uint step = 1u;
             step <= steps;
             ++step)
        {
            const float t =
                radius *
                ((float(step) - 1.0 + stepJitter) /
                 float(steps));

            const float3 hitPoint =
                surfacePosition +
                normal * 0.03 +
                direction * t;

            float2 hitUv;
            float queryViewDepth;

            if (!ProjectPoint(
                    hitPoint,
                    hitUv,
                    queryViewDepth))
            {
                break;
            }

            const float hitDepth =
                g_depth.SampleLevel(
                    g_depthSampler,
                    hitUv,
                    0).r;

            if (hitDepth <= 0.0)
            {
                previousDelta =
                    -1.0e20;
                previousT = t;
                continue;
            }

            const float sceneViewDepth =
                ReverseZViewDepth(
                    hitDepth);

            const float delta =
                queryViewDepth -
                sceneViewDepth;

            if (delta >= -thickness &&
                previousDelta < -thickness)
            {
                // Refine the crossing between the previous step and this one
                // so the hit distance (and the distance weight) is continuous
                // instead of snapping to a step.
                float hitDistance = t;
                {
                    float lo = previousT;
                    float hi = t;

                    [unroll]
                    for (int refine = 0; refine < 4; ++refine)
                    {
                        const float mid = 0.5 * (lo + hi);
                        const float3 midPoint =
                            surfacePosition +
                            normal * 0.03 +
                            direction * mid;

                        float2 midUv;
                        float midViewDepth;
                        if (!ProjectPoint(midPoint, midUv, midViewDepth))
                        {
                            break;
                        }

                        const float midDepth =
                            g_depth.SampleLevel(g_depthSampler, midUv, 0).r;
                        if (midDepth <= 0.0)
                        {
                            lo = mid;
                            continue;
                        }

                        const float midDelta =
                            midViewDepth - ReverseZViewDepth(midDepth);
                        if (midDelta >= -thickness)
                        {
                            hi = mid;
                            hitUv = midUv;
                        }
                        else
                        {
                            lo = mid;
                        }
                    }
                    hitDistance = hi;
                }

                const float4 hitMeta =
                    g_emissionClass.SampleLevel(
                        g_emissionSampler,
                        hitUv,
                        0);

                if (hitMeta.a > 0.0)
                {
                    const float3 hitNormal =
                        normalize(
                            g_normalMetallic.SampleLevel(
                                g_normalSampler,
                                hitUv,
                                0).xyz);

                    const float facing =
                        saturate(
                            dot(
                                hitNormal,
                                -direction));

                    const float sourceFacing =
                        saturate(
                            dot(
                                normal,
                                direction));

                    const float distanceWeight =
                        saturate(
                            1.0 -
                            hitDistance / radius);

                    const float weight =
                        sourceFacing *
                        lerp(
                            0.25,
                            1.0,
                            facing) *
                        max(
                            distanceWeight,
                            0.08);

                    float3 radiance =
                        max(
                            g_sceneColor.SampleLevel(
                                g_sceneSampler,
                                hitUv,
                                0).rgb,
                            0.0);

                    // A strongly emissive pixel is a small bright light that a
                    // handful of rays finds by chance: that is the speckle of
                    // GI noise. Such emitters light their surroundings
                    // analytically (mesh_render EmissiveLightRenderer), so
                    // only their non-emitted shading counts as bounce light.
                    const float3 hitEmission = max(hitMeta.rgb, 0.0);
                    if (max(hitEmission.r, max(hitEmission.g, hitEmission.b)) > 0.02)
                    {
                        radiance = max(radiance - hitEmission, 0.0);
                    }

                    const float particleT =
                        ParticleGridTransmittance(
                            surfaceFramePosition,
                            direction,
                            hitDistance);
                    const float3 particleEmission =
                        ParticleGridEmissionAlong(
                            surfaceFramePosition,
                            direction,
                            hitDistance);

                    accumulated +=
                        (radiance * particleT + particleEmission) *
                        weight;
                    accumulatedWeight +=
                        weight;
                    validRayCount +=
                        1.0;
                }

                resolved = true;
                break;
            }

            previousDelta = delta;
            previousT = t;
        }

        // The screen could not answer (the ray left the screen, or ran past
        // the screen-trace radius without a crossing): trace the mesh distance
        // field and read the hit's stored radiance instead.
        if (!resolved && g.sdfDimensionsEnable.w > 0.5)
        {
            float3 worldHit;
            float worldT;
            const float3 worldOrigin =
                surfacePosition + normal * (0.6 * g.sdfOriginVoxel.w);
            if (SdfTrace(worldOrigin, direction, kSdfGatherDistance, worldHit, worldT))
            {
                const float3 worldRadiance =
                    SdfFetchRadiance(worldHit, -direction);
                const float worldWeight =
                    saturate(dot(normal, direction)) *
                    max(saturate(1.0 - worldT / kSdfGatherDistance), 0.15);
                accumulated += worldRadiance * worldWeight;
                accumulatedWeight += worldWeight;
                validRayCount += 1.0;
            }
        }
    }
}

)"
R"(
// One ray per 2x2 quad: each thread traces a single ray from one pixel of its
// quad (a different pixel and direction every frame while the camera is still),
// shares it through groupshared memory, and every pixel of the quad is rebuilt
// from the 3x3 quad neighbourhood with bilinear-ish spatial weights times
// depth/normal bilateral weights. Tracing cost is a quarter of one ray per pixel
// and independent of the output resolution's pixel count.
groupshared float4 s_rad[64];
groupshared float4 s_geo[64];
groupshared float s_valid[64];
groupshared uint s_need[4];

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID, uint3 groupThread : SV_GroupThreadID)
{
    const uint local = groupThread.y * 8u + groupThread.x;
    const uint2 quadBase = dispatchId.xy * 2u;
    const bool insideQuad = quadBase.x < g.width && quadBase.y < g.height;

    s_rad[local] = 0.0;
    s_geo[local] = 0.0;
    s_valid[local] = 0.0;
    if (local < 4u)
    {
        s_need[local] = 0u;
    }

    if (insideQuad)
    {
        const uint frame =
            (g.historyCompatible & 1u) != 0u
                ? ((g.historyCompatible >> 8u) & 255u)
                : 0u;
        const uint pick =
            (frame + dispatchId.x * 3u + dispatchId.y * 5u) & 3u;

        [loop]
        for (uint attempt = 0u; attempt < 4u; ++attempt)
        {
            const uint k = (pick + attempt) & 3u;
            const uint2 pixel = quadBase + uint2(k & 1u, k >> 1u);
            if (pixel.x >= g.width || pixel.y >= g.height)
            {
                continue;
            }

            const float2 uv =
                (float2(pixel) + 0.5) /
                float2(g.width, g.height);
            const float4 emissionClass =
                g_emissionClass.SampleLevel(g_emissionSampler, uv, 0);
            const float depth =
                g_depth.SampleLevel(g_depthSampler, uv, 0).r;
            if (emissionClass.a <= 0.0 || depth <= 0.0)
            {
                continue;
            }

            const float4 normalMetallic =
                g_normalMetallic.SampleLevel(g_normalSampler, uv, 0);
            const float3 normal = normalize(normalMetallic.xyz);
            const float3 surfacePosition =
                ReconstructPosition(uv, depth);
            const float3 surfaceFramePosition =
                surfacePosition + g.cameraFrameParticleGrid.xyz;
            const float radius =
                max(g.depthRangeRadius.z, 0.05);
            const float thickness =
                max(g.depthRangeRadius.w, 0.001);
            const uint steps =
                clamp(g.stepsPerRay, 2u, 32u);

            float3 accumulated = 0.0;
            float accumulatedWeight = 0.0;
            float validRayCount = 0.0;
            const uint rayIndex =
                (dispatchId.x & 1u) | ((dispatchId.y & 1u) << 1u);

            TraceRay(
                pixel, normal, surfacePosition, surfaceFramePosition,
                radius, thickness, steps, rayIndex,
                accumulated, accumulatedWeight, validRayCount);

            s_rad[local] = float4(accumulated, accumulatedWeight);
            s_geo[local] = float4(normal, ReverseZViewDepth(depth));
            s_valid[local] = validRayCount;
            break;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    [loop]
    for (uint k = 0u; k < (insideQuad ? 4u : 0u); ++k)
    {
        const uint2 pixel = quadBase + uint2(k & 1u, k >> 1u);
        if (pixel.x >= g.width || pixel.y >= g.height)
        {
            continue;
        }

        const float2 uv =
            (float2(pixel) + 0.5) /
            float2(g.width, g.height);
        const float4 emissionClass =
            g_emissionClass.SampleLevel(g_emissionSampler, uv, 0);
        const float depth =
            g_depth.SampleLevel(g_depthSampler, uv, 0).r;
        if (emissionClass.a <= 0.0 || depth <= 0.0)
        {
            g_currentIndirect[pixel] = 0.0;
            g_currentMeta[pixel] = 0.0;
            continue;
        }

        const float4 baseRoughness =
            g_baseRoughness.SampleLevel(g_baseSampler, uv, 0);
        const float4 normalMetallic =
            g_normalMetallic.SampleLevel(g_normalSampler, uv, 0);
        const float3 normal = normalize(normalMetallic.xyz);
        const float centerDepth = ReverseZViewDepth(depth);

        const int sx = (k & 1u) != 0u ? 1 : -1;
        const int sy = (k >> 1u) != 0u ? 1 : -1;

        // Neighbourhood samples with their bilateral weights.
        float neighbourWeight[9];
        float3 neighbourRadiance[9];
        float neighbourRayWeight[9];
        float neighbourValid[9];
        float neighbourLuminance[9];
        [unroll]
        for (int i = 0; i < 9; ++i)
        {
            neighbourWeight[i] = 0.0;
            neighbourRadiance[i] = 0.0;
            neighbourRayWeight[i] = 0.0;
            neighbourValid[i] = 0.0;
            neighbourLuminance[i] = 0.0;
        }

        [unroll]
        for (int dy = -1; dy <= 1; ++dy)
        {
            [unroll]
            for (int dx = -1; dx <= 1; ++dx)
            {
                const int2 t = int2(groupThread.xy) + int2(dx, dy);
                if (any(t < 0) || any(t >= 8))
                {
                    continue;
                }
                const uint n = uint(t.y) * 8u + uint(t.x);
                const float4 geo = s_geo[n];
                if (geo.w <= 0.0)
                {
                    continue;
                }
                const float facing = saturate(dot(normal, geo.xyz));
                const float f2 = facing * facing;
                const float f4 = f2 * f2;
                const float normalWeight = f4 * f4 * f4;
                const float depthWeight =
                    exp(-abs(geo.w - centerDepth) /
                        (0.02 * centerDepth + 0.05));
                const float wx =
                    dx == 0 ? 3.0 : (dx == sx ? 1.0 : 0.15);
                const float wy =
                    dy == 0 ? 3.0 : (dy == sy ? 1.0 : 0.15);
                const int slot = (dy + 1) * 3 + (dx + 1);
                neighbourWeight[slot] = wx * wy * normalWeight * depthWeight;
                neighbourRadiance[slot] = s_rad[n].rgb;
                neighbourRayWeight[slot] = s_rad[n].a;
                neighbourValid[slot] = s_valid[n];
                neighbourLuminance[slot] =
                    s_rad[n].a > 1.0e-5
                        ? dot(s_rad[n].rgb / s_rad[n].a,
                              float3(0.2126, 0.7152, 0.0722))
                        : 0.0;
            }
        }

        // Firefly suppression: a single ray that hits a bright voxel or
        // surface among darker neighbours would otherwise be spread over the
        // whole 3x3 quad neighbourhood as a bright blob. Samples brighter than
        // a multiple of the neighbourhood's mean (taken without its brightest
        // member) are scaled down to that level.
        float lumSum = 0.0;
        float lumWeight = 0.0;
        float lumMax = 0.0;
        float lumMaxWeight = 0.0;
        [unroll]
        for (int j = 0; j < 9; ++j)
        {
            if (neighbourWeight[j] > 0.0 && neighbourRayWeight[j] > 1.0e-5)
            {
                lumSum += neighbourWeight[j] * neighbourLuminance[j];
                lumWeight += neighbourWeight[j];
                if (neighbourLuminance[j] > lumMax)
                {
                    lumMax = neighbourLuminance[j];
                    lumMaxWeight = neighbourWeight[j];
                }
            }
        }
        const float meanWithoutBrightest =
            lumWeight > lumMaxWeight + 1.0e-5
                ? (lumSum - lumMaxWeight * lumMax) /
                    (lumWeight - lumMaxWeight)
                : lumMax;
        const float luminanceCap = 4.0 * meanWithoutBrightest + 2.0e-5;

        float3 filteredRadiance = 0.0;
        float filteredWeight = 0.0;
        float filteredValid = 0.0;
        float filterNorm = 0.0;
        [unroll]
        for (int m = 0; m < 9; ++m)
        {
            const float scale =
                neighbourLuminance[m] > luminanceCap
                    ? luminanceCap / neighbourLuminance[m]
                    : 1.0;
            filteredRadiance +=
                neighbourWeight[m] * neighbourRadiance[m] * scale;
            filteredWeight += neighbourWeight[m] * neighbourRayWeight[m];
            filteredValid += neighbourWeight[m] * neighbourValid[m];
            filterNorm += neighbourWeight[m];
        }

        float confidence =
            saturate(filteredValid / max(filterNorm, 1.0e-5));

        {
            uint needIgnored;
            InterlockedAdd(s_need[0], 1u, needIgnored);
            // Visibly specular: mirror-like dielectrics or metals. A rough
            // dielectric reflects a few percent of an already blurred
            // environment, which the reflection passes do not need to add.
            if (baseRoughness.a < 0.25 || normalMetallic.w > 0.3)
            {
                InterlockedAdd(s_need[1], 1u, needIgnored);
            }
            if (confidence < 0.5)
            {
                InterlockedAdd(s_need[2], 1u, needIgnored);
            }
            // Mirror-like (the exact ray-traced reflections' domain,
            // HybridReflectionSettings::mirrorRoughness).
            if (baseRoughness.a <= 0.1)
            {
                InterlockedAdd(s_need[3], 1u, needIgnored);
            }
        }

        float3 indirect =
            filteredWeight > 1.0e-5
                ? filteredRadiance / filteredWeight
                : 0.0;

            // Diffuse-only final gather. Metallic surfaces are left for M20
            // reflection handling.
            indirect *=
                max(baseRoughness.rgb, 0.0) *
                (1.0 - saturate(normalMetallic.w)) /
                3.14159265;

            const float4 previousIndirect =
                g_previousIndirect.SampleLevel(
                    g_previousIndirectSampler,
                    uv,
                    0);

            const float4 previousMeta =
                g_previousMeta.SampleLevel(
                    g_previousMetaSampler,
                    uv,
                    0);

            const float depthDifference =
                abs(previousMeta.w - depth);

            const float normalAgreement =
                dot(
                    normalize(previousMeta.xyz),
                    normal);

            const bool historyValid =
                (g.historyCompatible & 1u) != 0u &&
                previousIndirect.a > 0.0 &&
                previousMeta.w > 0.0 &&
                depthDifference <=
                    max(g.gatherTuning.y, 0.0001) &&
                normalAgreement >=
                    g.gatherTuning.z;

            if (historyValid)
            {
                const float historyWeight =
                    saturate(g.gatherTuning.x) *
                    saturate(
                        min(
                            previousIndirect.a,
                            confidence) +
                        0.15);

                indirect =
                    lerp(
                        indirect,
                        previousIndirect.rgb,
                        historyWeight);

                confidence =
                    max(
                        confidence,
                        previousIndirect.a *
                            historyWeight);
            }

            indirect *=
                max(g.gatherTuning.w, 0.0);

            g_currentIndirect[pixel] =
                float4(
                    max(indirect, 0.0),
                    confidence);

            g_currentMeta[pixel] =
                float4(
                    normal,
                    depth);
    }

    GroupMemoryBarrierWithGroupSync();
    if (local < 4u && s_need[local] != 0u)
    {
        uint flushIgnored;
        g_needStats.InterlockedAdd(local * 4u, s_need[local], flushIgnored);
    }
}
)";


[[nodiscard]] std::string GatherSource()
{
    std::string source = kGatherCs;
    const std::string marker = "//SDF_TRACE_INCLUDE";
    source.replace(
        source.find(marker), marker.size(), kSdfTraceHlsl);
    return source;
}

constexpr const char* kCombineCs = R"(
[[vk::binding(0, 0)]]
RWTexture2D<float4> g_target : register(u0);

[[vk::binding(1, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_sceneColor : register(t1);
[[vk::binding(1, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sceneSampler : register(s1);

[[vk::binding(2, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_indirect : register(t2);
[[vk::binding(2, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_indirectSampler : register(s2);

struct Constants
{
    uint width;
    uint height;
    float intensity;
    uint debugMode;      // 1 = show the gather's coverage instead of adding it,
                         // 2 = show only the indirect light (no direct, sky or emission)
};

[[vk::push_constant]]
Constants g;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g.width ||
        dispatchId.y >= g.height)
    {
        return;
    }

    const uint2 pixel =
        dispatchId.xy;

    const float2 uv =
        (float2(pixel) + 0.5) /
        float2(g.width, g.height);

    const float4 scene =
        g_sceneColor.SampleLevel(
            g_sceneSampler,
            uv,
            0);

    const float4 indirect =
        g_indirect.SampleLevel(
            g_indirectSampler,
            uv,
            0);

    if (g.debugMode == 1u)
    {
        // Coverage view: R = gather confidence (alpha), G = brightness of the gathered light on a log
        // scale, B = 0; magenta = the gather returned nothing at all for this pixel (no light, no
        // confidence), which is where the surface gets no indirect light.
        const float luminance =
            dot(indirect.rgb, float3(0.2126, 0.7152, 0.0722));
        if (indirect.a <= 0.0 && luminance <= 0.0)
        {
            g_target[pixel] = float4(1.0, 0.0, 1.0, scene.a);
        }
        else
        {
            g_target[pixel] =
                float4(
                    saturate(indirect.a),
                    saturate(log2(1.0 + luminance * 64.0) / 6.0),
                    0.0,
                    scene.a);
        }
        return;
    }

    if (g.debugMode == 2u)
    {
        // GI only: what the final gather and the radiance-cache cascades add.
        g_target[pixel] =
            float4(
                max(indirect.rgb * max(g.intensity, 0.0), 0.0),
                scene.a);
        return;
    }

    g_target[pixel] =
        float4(
            max(
                scene.rgb +
                indirect.rgb *
                    max(g.intensity, 0.0),
                0.0),
            scene.a);
}
)";
} // namespace

bool CanReuseFinalGatherHistory(
    const LightingView& previous,
    const LightingView& current) noexcept
{
    if (!previous.frame ||
        !previous.body ||
        previous.frame != current.frame ||
        previous.body != current.body ||
        HasChange(
            current.change,
            LightingViewChange::CameraCut) ||
        HasChange(
            current.change,
            LightingViewChange::FrameChanged) ||
        HasChange(
            current.change,
            LightingViewChange::BodyChanged) ||
        HasChange(
            current.change,
            LightingViewChange::ProjectionChanged))
    {
        return false;
    }

    const math::Double3 cameraDelta =
        current.cameraPositionInFrameMeters -
        previous.cameraPositionInFrameMeters;

    if (math::LengthSquared(cameraDelta) >
        1.0e-10)
    {
        return false;
    }

    const f32 forwardAgreement =
        math::Dot(
            math::Normalize(previous.forward),
            math::Normalize(current.forward));

    const f32 upAgreement =
        math::Dot(
            math::Normalize(previous.up),
            math::Normalize(current.up));

    return
        std::isfinite(forwardAgreement) &&
        std::isfinite(upAgreement) &&
        forwardAgreement >= 0.999999F &&
        upAgreement >= 0.999999F;
}

ScreenSpaceFinalGatherRenderer::
ScreenSpaceFinalGatherRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto gather =
        compiler.Compile({
            .source = GatherSource(),
            .entryPoint = "main",
            .stage = shader::Stage::Compute,
            .debug = false
        });

    gatherPipeline_ =
        device.CreateComputePipeline({
            .computeShader = {
                .data = gather.bytecode.data(),
                .size = gather.bytecode.size()
            },
            .pushConstantDwords = 32U,
            .shaderResourceBuffers = 6U,
            .storageTextures = 2U,
            .sampledTextures = 7U
        });

    dummyParticleLightGrid_ = device.CreateBuffer({
        .sizeBytes = 2U * sizeof(std::array<u32,4U>),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource
    });
    std::memset(dummyParticleLightGrid_->Map(),0,static_cast<std::size_t>(dummyParticleLightGrid_->SizeBytes()));
    dummyParticleLightGrid_->Unmap();

    // One zeroed element bound in place of every field buffer when no mesh
    // distance field exists (the shader's enable flag stays 0).
    dummySdf_ = device.CreateBuffer({
        .sizeBytes = 4U * sizeof(f32),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource
    });
    std::memset(dummySdf_->Map(), 0, static_cast<std::size_t>(dummySdf_->SizeBytes()));
    dummySdf_->Unmap();

    const auto combine =
        compiler.Compile({
            .source = kCombineCs,
            .entryPoint = "main",
            .stage = shader::Stage::Compute,
            .debug = false
        });

    combinePipeline_ =
        device.CreateComputePipeline({
            .computeShader = {
                .data = combine.bytecode.data(),
                .size = combine.bytecode.size()
            },
            .pushConstantDwords = 4U,
            .shaderResourceBuffers = 0U,
            .storageTextures = 1U,
            .sampledTextures = 2U
        });
}

void ScreenSpaceFinalGatherRenderer::Gather(
    rhi::CommandList& commands,
    rhi::Texture& sceneColor,
    rhi::Texture& surfaceBaseRoughness,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& surfaceEmissionClass,
    rhi::Texture& depth,
    rhi::Texture& previousIndirect,
    rhi::Texture& previousMeta,
    rhi::Texture& currentIndirect,
    rhi::Texture& currentMeta,
    const u32 width,
    const u32 height,
    const LightingView& view,
    const bool historyCompatible,
    rhi::Buffer* const particleLightGrid,
    const ScreenSpaceFinalGatherSettings& settings,
    const SdfGatherInput* const sdf,
    rhi::Buffer* const needStats)
{
    if (width == 0U || height == 0U)
    {
        return;
    }

    const bool sdfAvailable =
        sdf != nullptr && sdf->distance != nullptr &&
        sdf->albedo != nullptr && sdf->normal != nullptr &&
        sdf->radiance != nullptr;
    const math::Double3 sdfOriginRelative = sdfAvailable
        ? sdf->originInFrameMeters - view.cameraPositionInFrameMeters
        : math::Double3{};

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    // The sample pattern rotates every frame while history is reusable, so a
    // longer blend converges to a smoother result for a still camera.
    const f32 temporalWeight = historyCompatible
        ? std::max(settings.temporalWeight, 0.94F)
        : settings.temporalWeight;

    const std::array<u32, 4> tuning{
        bits(std::clamp(
            temporalWeight,
            0.0F,
            0.99F)),
        bits(std::max(
            settings.historyDepthTolerance,
            0.0001F)),
        bits(std::clamp(
            settings.historyNormalThreshold,
            -1.0F,
            1.0F)),
        bits(std::max(
            settings.intensity,
            0.0F))
    };

    const std::array<u32, 32> fullConstants{
        width,
        height,
        std::clamp(settings.stepsPerRay, 2U, 32U),
        (historyCompatible ? 1U : 0U) | ((frameCounter_++ & 255U) << 8U),

        bits(view.forward.x),
        bits(view.forward.y),
        bits(view.forward.z),
        bits(
            static_cast<f32>(width) /
            static_cast<f32>(height)),

        bits(view.up.x),
        bits(view.up.y),
        bits(view.up.z),
        bits(std::tan(
            view.verticalFovRadians * 0.5F)),

        bits(std::max(view.nearPlaneMeters, 1.0e-5F)),
        bits(std::max(
            view.farPlaneMeters,
            view.nearPlaneMeters + 1.0e-4F)),
        bits(std::max(settings.radiusMeters, 0.05F)),
        bits(std::max(settings.thicknessMeters, 0.001F)),

        tuning[0],
        tuning[1],
        tuning[2],
        tuning[3],

        bits(static_cast<f32>(view.cameraPositionInFrameMeters.x)),
        bits(static_cast<f32>(view.cameraPositionInFrameMeters.y)),
        bits(static_cast<f32>(view.cameraPositionInFrameMeters.z)),
        bits(particleLightGrid != nullptr ? 1.0F : 0.0F),

        bits(static_cast<f32>(sdfOriginRelative.x)),
        bits(static_cast<f32>(sdfOriginRelative.y)),
        bits(static_cast<f32>(sdfOriginRelative.z)),
        bits(sdfAvailable ? sdf->voxelSize : 0.25F),

        bits(sdfAvailable ? static_cast<f32>(sdf->dimensions[0]) : 1.0F),
        bits(sdfAvailable ? static_cast<f32>(sdf->dimensions[1]) : 1.0F),
        bits(sdfAvailable ? static_cast<f32>(sdf->dimensions[2]) : 1.0F),
        bits(sdfAvailable ? 1.0F : 0.0F)
    };

    commands.SetComputePipeline(
        *gatherPipeline_);
    commands.SetComputeConstants(
        fullConstants);
    commands.SetComputeBuffer(
        0U,
        particleLightGrid != nullptr
            ? *particleLightGrid
            : *dummyParticleLightGrid_);
    commands.SetComputeBuffer(
        1U, sdfAvailable ? *sdf->distance : *dummySdf_);
    commands.SetComputeBuffer(
        2U, sdfAvailable ? *sdf->albedo : *dummySdf_);
    commands.SetComputeBuffer(
        3U, sdfAvailable ? *sdf->normal : *dummySdf_);
    commands.SetComputeBuffer(
        4U, sdfAvailable ? *sdf->radiance : *dummySdf_);
    commands.SetComputeBuffer(
        5U, needStats != nullptr ? *needStats : *dummySdf_);

    commands.SetComputeStorageTexture(
        0U,
        currentIndirect);
    commands.SetComputeStorageTexture(
        1U,
        currentMeta);

    commands.SetComputeTexture(
        0U,
        sceneColor);
    commands.SetComputeTexture(
        1U,
        surfaceBaseRoughness);
    commands.SetComputeTexture(
        2U,
        surfaceNormalMetallic);
    commands.SetComputeTexture(
        3U,
        surfaceEmissionClass);
    commands.SetComputeTexture(
        4U,
        depth);
    commands.SetComputeTexture(
        5U,
        previousIndirect);
    commands.SetComputeTexture(
        6U,
        previousMeta);

    // One thread per 2x2 quad; a group covers 16x16 pixels.
    commands.Dispatch(
        (((width + 1U) / 2U) + 7U) / 8U,
        (((height + 1U) / 2U) + 7U) / 8U,
        1U);
}

void ScreenSpaceFinalGatherRenderer::Combine(
    rhi::CommandList& commands,
    rhi::Texture& sceneColor,
    rhi::Texture& indirect,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    const f32 intensity,
    const bool coverageView,
    const bool indirectOnlyView)
{
    if (width == 0U || height == 0U)
    {
        return;
    }

    const std::array<u32, 4> constants{
        width,
        height,
        std::bit_cast<u32>(
            std::max(intensity, 0.0F)),
        coverageView ? 1U : (indirectOnlyView ? 2U : 0U)
    };

    commands.SetComputePipeline(
        *combinePipeline_);
    commands.SetComputeConstants(
        constants);
    commands.SetComputeStorageTexture(
        0U,
        target);
    commands.SetComputeTexture(
        0U,
        sceneColor);
    commands.SetComputeTexture(
        1U,
        indirect);

    commands.Dispatch(
        (width + 7U) / 8U,
        (height + 7U) / 8U,
        1U);
}
} // namespace orbit::lighting
