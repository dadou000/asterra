#include <orbit/lighting/ExactReflectionQueryRenderer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace orbit::lighting
{
namespace
{
constexpr const char* kBuildCs = R"(
[[vk::binding(0, 0)]]
RWByteAddressBuffer g_queries : register(u0);
[[vk::binding(1, 0)]]
RWByteAddressBuffer g_pixelMap : register(u1);
[[vk::binding(2, 0)]]
RWByteAddressBuffer g_counter : register(u2);

[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_baseRoughness : register(t3);
[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_baseSampler : register(s3);

[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_normalMetallic : register(t4);
[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_normalSampler : register(s4);

[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth : register(t5);
[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler : register(s5);

struct Constants
{
    uint width;
    uint height;
    uint maximumQueries;
    uint screenSteps;

    float4 forwardAspect;
    float4 upTanHalfFov;
    float4 depthTrace;
    float4 tuning;
    float4 currentToSceneOrigin;
};

[[vk::push_constant]]
Constants g;

float ReverseZViewDepth(float depth)
{
    const float nearPlane = max(g.depthTrace.x, 1.0e-5);
    const float farPlane = max(g.depthTrace.y, nearPlane + 1.0e-4);
    return nearPlane * farPlane /
        max(depth * (farPlane - nearPlane) + nearPlane, 1.0e-6);
}

void CameraBasis(
    out float3 forward,
    out float3 right,
    out float3 up,
    out float aspect,
    out float tanHalfFov)
{
    forward = normalize(g.forwardAspect.xyz);
    const float3 requestedUp = normalize(g.upTanHalfFov.xyz);
    right = normalize(cross(forward, requestedUp));
    up = normalize(cross(right, forward));
    aspect = max(g.forwardAspect.w, 0.001);
    tanHalfFov = max(g.upTanHalfFov.w, 0.001);
}

float3 ViewRay(float2 uv)
{
    float3 forward, right, up;
    float aspect, tanHalfFov;
    CameraBasis(forward, right, up, aspect, tanHalfFov);

    const float2 ndc = {
        uv.x * 2.0 - 1.0,
        1.0 - uv.y * 2.0
    };

    return normalize(
        forward +
        right * (ndc.x * aspect * tanHalfFov) +
        up * (ndc.y * tanHalfFov));
}

float3 ReconstructPosition(float2 uv, float depth)
{
    const float3 ray = ViewRay(uv);
    const float3 forward = normalize(g.forwardAspect.xyz);
    const float viewDepth = ReverseZViewDepth(depth);

    return ray *
        (viewDepth /
         max(dot(ray, forward), 1.0e-5));
}

bool ProjectPoint(
    float3 position,
    out float2 uv,
    out float viewDepth)
{
    float3 forward, right, up;
    float aspect, tanHalfFov;
    CameraBasis(forward, right, up, aspect, tanHalfFov);

    viewDepth = dot(position, forward);

    if (viewDepth <= max(g.depthTrace.x, 1.0e-5))
    {
        uv = 0.0;
        return false;
    }

    const float2 ndc = {
        dot(position, right) / (viewDepth * tanHalfFov * aspect),
        dot(position, up) / (viewDepth * tanHalfFov)
    };

    uv = {
        ndc.x * 0.5 + 0.5,
        0.5 - ndc.y * 0.5
    };

    return all(uv >= 0.0) && all(uv <= 1.0);
}

bool HasScreenHit(
    float3 position,
    float3 direction)
{
    const float traceRadius = max(g.depthTrace.z, 0.1);
    const float thickness = max(g.depthTrace.w, 0.001);
    const uint steps = max(g.screenSteps, 2u);

    float previousDelta = -1.0e20;

    [loop]
    for (uint step = 1u;
         step <= steps;
         ++step)
    {
        const float t =
            traceRadius *
            (float(step) / float(steps));

        const float3 queryPosition =
            position +
            direction * t;

        float2 hitUv;
        float queryViewDepth;

        if (!ProjectPoint(
                queryPosition,
                hitUv,
                queryViewDepth))
        {
            return false;
        }

        const float hitDepth =
            g_depth.SampleLevel(
                g_depthSampler,
                hitUv,
                0).r;

        if (hitDepth <= 0.0)
        {
            previousDelta = -1.0e20;
            continue;
        }

        const float sceneViewDepth =
            ReverseZViewDepth(hitDepth);

        const float delta =
            queryViewDepth -
            sceneViewDepth;

        if (delta >= -thickness &&
            previousDelta < -thickness)
        {
            return true;
        }

        previousDelta = delta;
    }

    return false;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g.width ||
        dispatchId.y >= g.height)
    {
        return;
    }

    const uint2 pixel = dispatchId.xy;
    const float2 uv =
        (float2(pixel) + 0.5) /
        float2(g.width, g.height);

    const float depth =
        g_depth.SampleLevel(
            g_depthSampler,
            uv,
            0).r;

    if (depth <= 0.0)
    {
        return;
    }

    const float roughness =
        saturate(
            g_baseRoughness.SampleLevel(
                g_baseSampler,
                uv,
                0).a);

    if (roughness > g.tuning.x)
    {
        return;
    }

    const float3 normal =
        normalize(
            g_normalMetallic.SampleLevel(
                g_normalSampler,
                uv,
                0).xyz);

    const float3 viewRay =
        ViewRay(uv);

    const float3 reflectionDirection =
        normalize(
            reflect(viewRay, normal));

    const float3 position =
        ReconstructPosition(
            uv,
            depth) +
        normal * 0.03;

    if (HasScreenHit(
            position,
            reflectionDirection))
    {
        return;
    }

    uint queryIndex;
    g_counter.InterlockedAdd(
        0u,
        1u,
        queryIndex);

    if (queryIndex >= g.maximumQueries)
    {
        return;
    }

    const uint queryBase =
        queryIndex * 48u;

    g_queries.Store4(
        queryBase + 0u,
        asuint(
            float4(
                position +
                    g.currentToSceneOrigin.xyz,
                0.03)));

    g_queries.Store4(
        queryBase + 16u,
        asuint(
            float4(
                reflectionDirection,
                max(
                    g.depthTrace.z,
                    0.1))));

    // requirements:
    // x maximum nominal error
    // y minimum confidence
    // z importance
    // w flags
    g_queries.Store4(
        queryBase + 32u,
        asuint(
            float4(
                g.tuning.y,
                g.tuning.z,
                1.0,
                asfloat(1u | 2u))));

    g_pixelMap.Store(
        queryIndex * 4u,
        pixel.y * g.width +
            pixel.x);
}
)";

constexpr const char* kResolveCs = R"(
struct GpuRadianceCell
{
    float4 irradiance0;
    float4 irradianceX;
    float4 irradianceY;
    float4 irradianceZ;
};

struct GpuRadianceLevelInfo
{
    float4 centerCellSize;
    uint4 moduloAxis;
    uint4 offsetCountLevel;
};

[[vk::binding(0, 0)]]
ByteAddressBuffer g_results : register(t0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_pixelMap : register(t1);
[[vk::binding(2, 0)]]
StructuredBuffer<GpuRadianceCell> g_cells : register(t2);
[[vk::binding(3, 0)]]
StructuredBuffer<GpuRadianceLevelInfo> g_levels : register(t3);

[[vk::binding(4, 0)]]
RWTexture2D<float4> g_target : register(u4);

[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_baseRoughness : register(t5);
[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_baseSampler : register(s5);

[[vk::binding(6, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_normalMetallic : register(t6);
[[vk::binding(6, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_normalSampler : register(s6);

struct Constants
{
    uint maximumQueries;
    uint width;
    uint height;
    uint levelCount;
    float cacheStrength;
    float sceneToCurrentX;
    float sceneToCurrentY;
    float sceneToCurrentZ;
};

[[vk::push_constant]]
Constants g;

int PositiveModulo(int value, int modulus)
{
    const int r = value % modulus;
    return r < 0 ? r + modulus : r;
}

float3 SampleCache(
    float3 position,
    float3 direction)
{
    [loop]
    for (uint levelIndex = 0u;
         levelIndex < g.levelCount;
         ++levelIndex)
    {
        const GpuRadianceLevelInfo level =
            g_levels[levelIndex];

        const float cellSize =
            max(level.centerCellSize.w, 1.0e-5);
        const int axis =
            int(level.moduloAxis.w);

        if (axis <= 0)
        {
            continue;
        }

        const int3 delta =
            int3(
                round(
                    (position -
                     level.centerCellSize.xyz) /
                    cellSize));

        const int halfAxis =
            axis / 2;

        if (any(abs(delta) > halfAxis))
        {
            continue;
        }

        const int px =
            PositiveModulo(
                int(level.moduloAxis.x) +
                    delta.x,
                axis);
        const int py =
            PositiveModulo(
                int(level.moduloAxis.y) +
                    delta.y,
                axis);
        const int pz =
            PositiveModulo(
                int(level.moduloAxis.z) +
                    delta.z,
                axis);

        const uint localIndex =
            uint(px) +
            uint(axis) *
                (uint(py) +
                 uint(axis) * uint(pz));

        if (localIndex >=
            level.offsetCountLevel.y)
        {
            continue;
        }

        const GpuRadianceCell cell =
            g_cells[
                level.offsetCountLevel.x +
                localIndex];

        if (cell.irradiance0.w <= 0.5)
        {
            continue;
        }

        const float3 d =
            normalize(direction);

        return max(
            cell.irradiance0.rgb +
            cell.irradianceX.rgb * d.x +
            cell.irradianceY.rgb * d.y +
            cell.irradianceZ.rgb * d.z,
            0.0);
    }

    return 0.0;
}

float3 FresnelSchlick(
    float cosTheta,
    float3 f0)
{
    return
        f0 +
        (1.0 - f0) *
        pow(
            1.0 - saturate(cosTheta),
            5.0);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint index =
        dispatchId.x;

    if (index >= g.maximumQueries)
    {
        return;
    }

    const uint packedPixel =
        g_pixelMap.Load(index * 4u);

    if (packedPixel == 0xFFFFFFFFu)
    {
        return;
    }

    const uint2 pixel = {
        packedPixel % g.width,
        packedPixel / g.width
    };

    if (pixel.x >= g.width ||
        pixel.y >= g.height)
    {
        return;
    }

    const uint base =
        index * 48u;

    const uint resolution =
        g_results.Load(base + 0u);

    if (resolution != 1u)
    {
        return;
    }

    const float confidence =
        asfloat(
            g_results.Load(
                base + 8u));

    if (confidence <= 0.0)
    {
        return;
    }

    const float3 hitPosition =
        asfloat(
            g_results.Load3(
                base + 16u)) +
        float3(
            g.sceneToCurrentX,
            g.sceneToCurrentY,
            g.sceneToCurrentZ);

    const float3 hitNormal =
        normalize(
            asfloat(
                g_results.Load3(
                    base + 32u)));

    const float4 baseRoughness =
        g_baseRoughness.Load(
            int3(pixel, 0));

    const float4 normalMetallic =
        g_normalMetallic.Load(
            int3(pixel, 0));

    const float3 sourceNormal =
        normalize(normalMetallic.xyz);

    const float metallic =
        saturate(normalMetallic.w);

    // The hit is shaded from Orbit's common broad radiance representation.
    // Exact visibility determines *where* the mirror ray lands; radiance
    // authority remains shared with RT-off paths.
    const float3 hitRadiance =
        SampleCache(
            hitPosition,
            -hitNormal) *
        max(g.cacheStrength, 0.0);

    const float3 dielectricF0 =
        float3(0.04, 0.04, 0.04);

    const float3 f0 =
        lerp(
            dielectricF0,
            max(baseRoughness.rgb, 0.0),
            metallic);

    const float3 fresnel =
        FresnelSchlick(
            saturate(sourceNormal.z),
            f0);

    const float3 contribution =
        hitRadiance *
        fresnel *
        confidence;

    float4 target =
        g_target[pixel];

    target.rgb =
        max(
            target.rgb +
            contribution,
            0.0);

    g_target[pixel] =
        target;
}
)";
} // namespace

ExactReflectionQueryRenderer::
ExactReflectionQueryRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto build =
        compiler.Compile({
            .source = kBuildCs,
            .entryPoint = "main",
            .stage = shader::Stage::Compute,
            .debug = false
        });

    buildPipeline_ =
        device.CreateComputePipeline({
            .computeShader = {
                .data = build.bytecode.data(),
                .size = build.bytecode.size()
            },
            .pushConstantDwords = 24U,
            .shaderResourceBuffers = 3U,
            .storageTextures = 0U,
            .sampledTextures = 3U
        });

    const auto resolve =
        compiler.Compile({
            .source = kResolveCs,
            .entryPoint = "main",
            .stage = shader::Stage::Compute,
            .debug = false
        });

    resolvePipeline_ =
        device.CreateComputePipeline({
            .computeShader = {
                .data = resolve.bytecode.data(),
                .size = resolve.bytecode.size()
            },
            .pushConstantDwords = 8U,
            .shaderResourceBuffers = 4U,
            .storageTextures = 1U,
            .sampledTextures = 2U
        });
}

void ExactReflectionQueryRenderer::BuildQueries(
    rhi::CommandList& commands,
    rhi::Texture& surfaceBaseRoughness,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& depth,
    rhi::Buffer& queries,
    rhi::Buffer& pixelMap,
    rhi::Buffer& counter,
    const u32 maximumQueries,
    const u32 width,
    const u32 height,
    const LightingView& view,
    const math::Float3 currentToSceneOriginMeters,
    const f32 mirrorRoughness,
    const f32 traceRadiusMeters,
    const f32 thicknessMeters,
    const u32 screenSteps)
{
    if (maximumQueries == 0U ||
        width == 0U ||
        height == 0U)
    {
        return;
    }

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    const std::array<u32, 24> constants{
        width,
        height,
        maximumQueries,
        std::max(screenSteps, 2U),

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
        bits(std::max(traceRadiusMeters, 0.1F)),
        bits(std::max(thicknessMeters, 0.001F)),

        bits(std::clamp(mirrorRoughness, 0.0F, 1.0F)),
        bits(0.25F),
        bits(0.50F),
        0U,

        bits(currentToSceneOriginMeters.x),
        bits(currentToSceneOriginMeters.y),
        bits(currentToSceneOriginMeters.z),
        0U
    };

    commands.SetComputePipeline(*buildPipeline_);
    commands.SetComputeConstants(constants);

    commands.SetComputeBuffer(0U, queries);
    commands.SetComputeBuffer(1U, pixelMap);
    commands.SetComputeBuffer(2U, counter);

    commands.SetComputeTexture(0U, surfaceBaseRoughness);
    commands.SetComputeTexture(1U, surfaceNormalMetallic);
    commands.SetComputeTexture(2U, depth);

    commands.Dispatch(
        (width + 7U) / 8U,
        (height + 7U) / 8U,
        1U);
}

void ExactReflectionQueryRenderer::ResolveResults(
    rhi::CommandList& commands,
    rhi::Texture& targetSceneColor,
    rhi::Texture& surfaceBaseRoughness,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Buffer& results,
    rhi::Buffer& pixelMap,
    rhi::Buffer& radianceCells,
    rhi::Buffer& radianceLevels,
    const u32 radianceLevelCount,
    const u32 maximumQueries,
    const u32 width,
    const u32 height,
    const math::Float3 sceneToCurrentOriginMeters,
    const f32 cacheStrength)
{
    if (maximumQueries == 0U ||
        width == 0U ||
        height == 0U ||
        radianceLevelCount == 0U)
    {
        return;
    }

    const std::array<u32, 8> constants{
        maximumQueries,
        width,
        height,
        radianceLevelCount,
        std::bit_cast<u32>(
            std::max(cacheStrength, 0.0F)),
        std::bit_cast<u32>(
            sceneToCurrentOriginMeters.x),
        std::bit_cast<u32>(
            sceneToCurrentOriginMeters.y),
        std::bit_cast<u32>(
            sceneToCurrentOriginMeters.z)
    };

    commands.SetComputePipeline(*resolvePipeline_);
    commands.SetComputeConstants(constants);

    commands.SetComputeBuffer(0U, results);
    commands.SetComputeBuffer(1U, pixelMap);
    commands.SetComputeBuffer(2U, radianceCells);
    commands.SetComputeBuffer(3U, radianceLevels);

    commands.SetComputeStorageTexture(
        0U,
        targetSceneColor);

    commands.SetComputeTexture(
        0U,
        surfaceBaseRoughness);
    commands.SetComputeTexture(
        1U,
        surfaceNormalMetallic);

    commands.Dispatch(
        (maximumQueries + 63U) / 64U,
        1U,
        1U);
}
} // namespace orbit::lighting
