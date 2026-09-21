#include <orbit/lighting/RadianceCacheSampler.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace orbit::lighting
{
namespace
{
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
StructuredBuffer<GpuRadianceCell> g_cells : register(t0);

[[vk::binding(1, 0)]]
StructuredBuffer<GpuRadianceLevelInfo> g_levels : register(t1);

[[vk::binding(2, 0)]]
RWTexture2D<float4> g_indirect : register(u2);

[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_baseRoughness : register(t3);
[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_baseSampler : register(s1);

[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_normalMetallic : register(t4);
[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_normalSampler : register(s2);

[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth : register(t5);
[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler : register(s3);

struct Constants
{
    uint width;
    uint height;
    uint levelCount;
    float cacheStrength;

    float4 forwardAspect;
    float4 upTanHalfFov;
    float4 depthRange;
};

[[vk::push_constant]]
Constants g;

float ReverseZViewDepth(float depth)
{
    const float nearPlane =
        max(g.depthRange.x, 1.0e-5);
    const float farPlane =
        max(g.depthRange.y, nearPlane + 1.0e-4);

    return
        nearPlane * farPlane /
        max(
            depth * (farPlane - nearPlane) +
                nearPlane,
            1.0e-6);
}

float3 ReconstructPosition(
    float2 uv,
    float depth)
{
    const float3 forward =
        normalize(g.forwardAspect.xyz);
    const float3 requestedUp =
        normalize(g.upTanHalfFov.xyz);
    const float3 right =
        normalize(cross(forward, requestedUp));
    const float3 up =
        normalize(cross(right, forward));

    const float aspect =
        max(g.forwardAspect.w, 0.001);
    const float tanHalfFov =
        max(g.upTanHalfFov.w, 0.001);

    const float2 ndc = {
        uv.x * 2.0 - 1.0,
        1.0 - uv.y * 2.0
    };

    const float3 ray =
        normalize(
            forward +
            right *
                (ndc.x * aspect * tanHalfFov) +
            up *
                (ndc.y * tanHalfFov));

    const float viewDepth =
        ReverseZViewDepth(depth);

    const float rayForward =
        max(dot(ray, forward), 1.0e-5);

    return
        ray *
        (viewDepth / rayForward);
}

int PositiveModulo(int value, int modulus)
{
    const int result = value % modulus;
    return
        result < 0
            ? result + modulus
            : result;
}

float3 EvaluateCell(
    GpuRadianceCell cell,
    float3 normal)
{
    return max(
        cell.irradiance0.rgb +
        cell.irradianceX.rgb * normal.x +
        cell.irradianceY.rgb * normal.y +
        cell.irradianceZ.rgb * normal.z,
        0.0);
}

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

    const float depth =
        g_depth.SampleLevel(
            g_depthSampler,
            uv,
            0).r;

    if (depth <= 0.0)
    {
        return;
    }

    const float4 baseRoughness =
        g_baseRoughness.SampleLevel(
            g_baseSampler,
            uv,
            0);

    const float4 normalMetallic =
        g_normalMetallic.SampleLevel(
            g_normalSampler,
            uv,
            0);

    const float3 normal =
        normalize(normalMetallic.xyz);

    const float3 position =
        ReconstructPosition(
            uv,
            depth);

    float3 cacheIrradiance = 0.0;
    float cacheValid = 0.0;

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

        const float3 relative =
            (position -
             level.centerCellSize.xyz) /
            cellSize;

        const int3 delta =
            int3(round(relative));

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

        cacheIrradiance =
            EvaluateCell(
                cell,
                normal);
        cacheValid = 1.0;
        break;
    }

    if (cacheValid <= 0.0)
    {
        return;
    }

    const float metallic =
        saturate(normalMetallic.w);

    const float3 cacheDiffuse =
        cacheIrradiance *
        max(baseRoughness.rgb, 0.0) *
        (1.0 - metallic) /
        3.14159265 *
        max(g.cacheStrength, 0.0);

    const float4 screen =
        g_indirect[pixel];

    const float screenConfidence =
        saturate(screen.a);

    const float3 resolved =
        lerp(
            cacheDiffuse,
            max(screen.rgb, 0.0),
            screenConfidence);

    g_indirect[pixel] =
        float4(
            resolved,
            max(
                screenConfidence,
                cacheValid));
}
)";
} // namespace

RadianceCacheSampler::RadianceCacheSampler(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto shader =
        compiler.Compile({
            .source = kResolveCs,
            .entryPoint = "main",
            .stage = shader::Stage::Compute,
            .debug = false
        });

    pipeline_ =
        device.CreateComputePipeline({
            .computeShader = {
                .data = shader.bytecode.data(),
                .size = shader.bytecode.size()
            },
            .pushConstantDwords = 16U,
            .shaderResourceBuffers = 2U,
            .storageTextures = 1U,
            .sampledTextures = 3U
        });
}

void RadianceCacheSampler::ResolveFallback(
    rhi::CommandList& commands,
    rhi::Texture& indirect,
    rhi::Texture& surfaceBaseRoughness,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& depth,
    rhi::Buffer& radianceCells,
    rhi::Buffer& radianceLevels,
    const u32 levelCount,
    const u32 width,
    const u32 height,
    const LightingView& view,
    const f32 cacheStrength)
{
    if (width == 0U ||
        height == 0U ||
        levelCount == 0U)
    {
        return;
    }

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    const std::array<u32, 16> constants{
        width,
        height,
        levelCount,
        bits(std::max(cacheStrength, 0.0F)),

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
            view.verticalFovRadians *
            0.5F)),

        bits(std::max(
            view.nearPlaneMeters,
            1.0e-5F)),
        bits(std::max(
            view.farPlaneMeters,
            view.nearPlaneMeters +
                1.0e-4F)),
        0U,
        0U
    };

    commands.SetComputePipeline(
        *pipeline_);
    commands.SetComputeConstants(
        constants);

    commands.SetComputeStorageTexture(
        0U,
        indirect);

    commands.SetComputeTexture(
        0U,
        surfaceBaseRoughness);
    commands.SetComputeTexture(
        1U,
        surfaceNormalMetallic);
    commands.SetComputeTexture(
        2U,
        depth);

    commands.SetComputeBuffer(
        0U,
        radianceCells);
    commands.SetComputeBuffer(
        1U,
        radianceLevels);

    commands.Dispatch(
        (width + 7U) / 8U,
        (height + 7U) / 8U,
        1U);
}
} // namespace orbit::lighting
