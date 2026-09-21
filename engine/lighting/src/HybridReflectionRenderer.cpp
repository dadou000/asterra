#include <orbit/lighting/HybridReflectionRenderer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace orbit::lighting
{
namespace
{
constexpr const char* kCs = R"(
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
RWTexture2D<float4> g_target : register(u2);

[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_sceneColor : register(t3);
[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sceneSampler : register(s3);

[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_baseRoughness : register(t4);
[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_baseSampler : register(s4);

[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_normalMetallic : register(t5);
[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_normalSampler : register(s5);

[[vk::binding(6, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth : register(t6);
[[vk::binding(6, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler : register(s6);

struct Constants
{
    uint width;
    uint height;
    uint levelCount;
    uint maximumSteps;

    float4 forwardAspect;
    float4 upTanHalfFov;
    float4 depthTrace;
    float4 reflectionTuning;
};

[[vk::push_constant]]
Constants g;

float ReverseZViewDepth(float depth)
{
    const float nearPlane =
        max(g.depthTrace.x, 1.0e-5);
    const float farPlane =
        max(g.depthTrace.y, nearPlane + 1.0e-4);

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

    return normalize(
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

    return
        ray *
        (viewDepth /
         max(dot(ray, forward), 1.0e-5));
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
        max(g.depthTrace.x, 1.0e-5))
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

    uv = {
        ndc.x * 0.5 + 0.5,
        0.5 - ndc.y * 0.5
    };

    return
        all(uv >= 0.0) &&
        all(uv <= 1.0);
}

int PositiveModulo(
    int value,
    int modulus)
{
    const int result =
        value % modulus;

    return
        result < 0
            ? result + modulus
            : result;
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

    const float depth =
        g_depth.SampleLevel(
            g_depthSampler,
            uv,
            0).r;

    if (depth <= 0.0)
    {
        g_target[pixel] = scene;
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

    const float roughness =
        saturate(baseRoughness.a);
    const float metallic =
        saturate(normalMetallic.a);
    const float3 normal =
        normalize(normalMetallic.xyz);

    const float3 viewRay =
        ViewRay(uv);
    const float3 viewToCamera =
        -viewRay;

    const float3 reflectionDirection =
        normalize(
            reflect(
                viewRay,
                normal));

    const float3 position =
        ReconstructPosition(
            uv,
            depth);

    float3 reflectedRadiance = 0.0;
    float screenConfidence = 0.0;

    const float maxScreenRoughness =
        max(g.reflectionTuning.x, 0.0);

    if (roughness <= maxScreenRoughness)
    {
        const float traceRadius =
            max(g.depthTrace.z, 0.1) *
            lerp(
                1.0,
                0.35,
                saturate(
                    roughness /
                    max(
                        maxScreenRoughness,
                        1.0e-4)));

        const float thickness =
            max(g.depthTrace.w, 0.001);

        const uint steps =
            max(g.maximumSteps, 2u);

        float previousDelta =
            -1.0e20;

        [loop]
        for (uint step = 1u;
             step <= steps;
             ++step)
        {
            const float t =
                traceRadius *
                (float(step) /
                 float(steps));

            const float3 queryPosition =
                position +
                normal * 0.03 +
                reflectionDirection * t;

            float2 hitUv;
            float queryViewDepth;

            if (!ProjectPoint(
                    queryPosition,
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
                reflectedRadiance =
                    max(
                        g_sceneColor.SampleLevel(
                            g_sceneSampler,
                            hitUv,
                            roughness * 4.0).rgb,
                        0.0);

                const float edge =
                    min(
                        min(hitUv.x, 1.0 - hitUv.x),
                        min(hitUv.y, 1.0 - hitUv.y));

                screenConfidence =
                    saturate(
                        edge / 0.04);
                break;
            }

            previousDelta = delta;
        }
    }

    const float3 cacheRadiance =
        SampleCache(
            position,
            reflectionDirection) *
        max(g.reflectionTuning.z, 0.0);

    reflectedRadiance =
        lerp(
            cacheRadiance,
            reflectedRadiance,
            screenConfidence);

    const float3 dielectricF0 =
        float3(0.04, 0.04, 0.04);

    const float3 f0 =
        lerp(
            dielectricF0,
            max(baseRoughness.rgb, 0.0),
            metallic);

    const float3 fresnel =
        FresnelSchlick(
            dot(
                normal,
                viewToCamera),
            f0);

    const float roughnessAttenuation =
        1.0 -
        0.55 *
            roughness *
            roughness;

    const float3 contribution =
        reflectedRadiance *
        fresnel *
        max(
            roughnessAttenuation,
            0.0);

    g_target[pixel] =
        float4(
            max(
                scene.rgb +
                contribution,
                0.0),
            scene.a);
}
)";
} // namespace

HybridReflectionRenderer::
HybridReflectionRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto shader =
        compiler.Compile({
            .source = kCs,
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
            .pushConstantDwords = 20U,
            .shaderResourceBuffers = 2U,
            .storageTextures = 1U,
            .sampledTextures = 4U
        });
}

void HybridReflectionRenderer::Resolve(
    rhi::CommandList& commands,
    rhi::Texture& sceneColor,
    rhi::Texture& surfaceBaseRoughness,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& depth,
    rhi::Buffer& radianceCells,
    rhi::Buffer& radianceLevels,
    const u32 radianceLevelCount,
    rhi::Texture& targetSceneColor,
    const u32 width,
    const u32 height,
    const LightingView& view,
    const f32 qualityScale,
    const HybridReflectionSettings& settings)
{
    if (width == 0U ||
        height == 0U ||
        radianceLevelCount == 0U)
    {
        return;
    }

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    const f32 quality =
        std::clamp(
            qualityScale,
            0.0F,
            1.0F);

    const u32 steps =
        std::clamp(
            static_cast<u32>(
                std::lround(
                    2.0F +
                    quality *
                    static_cast<f32>(
                        std::max(
                            settings.maximumSteps,
                            2U) -
                        2U))),
            2U,
            std::max(
                settings.maximumSteps,
                2U));

    const std::array<u32, 20> constants{
        width,
        height,
        radianceLevelCount,
        steps,

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
        bits(std::max(
            settings.traceRadiusMeters,
            0.1F)),
        bits(std::max(
            settings.thicknessMeters,
            0.001F)),

        bits(std::clamp(
            settings.maximumScreenTraceRoughness,
            0.0F,
            1.0F)),
        bits(std::clamp(
            settings.mirrorRoughness,
            0.0F,
            1.0F)),
        bits(std::max(
            settings.cacheStrength,
            0.0F)),
        bits(quality)
    };

    commands.SetComputePipeline(
        *pipeline_);
    commands.SetComputeConstants(
        constants);

    commands.SetComputeBuffer(
        0U,
        radianceCells);
    commands.SetComputeBuffer(
        1U,
        radianceLevels);

    commands.SetComputeStorageTexture(
        0U,
        targetSceneColor);

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
        depth);

    commands.Dispatch(
        (width + 7U) / 8U,
        (height + 7U) / 8U,
        1U);
}
} // namespace orbit::lighting
