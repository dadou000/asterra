#include <orbit/lighting/ScreenSpaceFinalGather.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace orbit::lighting
{
namespace
{
constexpr const char* kGatherCs = R"(
[[vk::binding(0, 0)]]
RWTexture2D<float4> g_currentIndirect : register(u0);
[[vk::binding(1, 0)]]
RWTexture2D<float4> g_currentMeta : register(u1);

[[vk::binding(2, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_sceneColor : register(t2);
[[vk::binding(2, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sceneSampler : register(s2);

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
Texture2D g_emissionClass : register(t5);
[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_emissionSampler : register(s5);

[[vk::binding(6, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth : register(t6);
[[vk::binding(6, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler : register(s6);

[[vk::binding(7, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_previousIndirect : register(t7);
[[vk::binding(7, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_previousIndirectSampler : register(s7);

[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_previousMeta : register(t8);
[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_previousMetaSampler : register(s8);

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
};

[[vk::push_constant]]
Constants g;

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

    uv = {
        ndc.x * 0.5 + 0.5,
        0.5 - ndc.y * 0.5
    };

    return
        all(uv >= 0.0) &&
        all(uv <= 1.0);
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

    const float4 emissionClass =
        g_emissionClass.SampleLevel(
            g_emissionSampler,
            uv,
            0);

    const float depth =
        g_depth.SampleLevel(
            g_depthSampler,
            uv,
            0).r;

    if (emissionClass.a <= 0.0 ||
        depth <= 0.0)
    {
        g_currentIndirect[pixel] = 0.0;
        g_currentMeta[pixel] = 0.0;
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

    const float3 surfacePosition =
        ReconstructPosition(
            uv,
            depth);

    const float radius =
        max(g.depthRangeRadius.z, 0.05);

    const float thickness =
        max(g.depthRangeRadius.w, 0.001);

    const uint steps =
        clamp(g.stepsPerRay, 2u, 32u);

    const float randomRotation =
        Hash12(float2(pixel)) *
        6.28318530718;

    float3 accumulated = 0.0;
    float accumulatedWeight = 0.0;
    float validRayCount = 0.0;

    [unroll]
    for (uint rayIndex = 0u;
         rayIndex < 4u;
         ++rayIndex)
    {
        const float angle =
            randomRotation +
            (float(rayIndex) + 0.5) *
                1.57079632679;

        const float elevation =
            lerp(
                0.28,
                0.72,
                frac(
                    float(rayIndex) *
                        0.61803398875 +
                    Hash12(
                        float2(pixel) +
                        float2(17.0, 41.0))));

        const float3 direction =
            TangentDirection(
                normal,
                angle,
                elevation);

        float previousDelta =
            -1.0e20;
        bool resolved = false;

        [loop]
        for (uint step = 1u;
             step <= steps;
             ++step)
        {
            const float t =
                radius *
                (float(step) /
                 float(steps));

            const float3 point =
                surfacePosition +
                normal * 0.03 +
                direction * t;

            float2 hitUv;
            float queryViewDepth;

            if (!ProjectPoint(
                    point,
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
                            t / radius);

                    const float weight =
                        sourceFacing *
                        lerp(
                            0.25,
                            1.0,
                            facing) *
                        max(
                            distanceWeight,
                            0.08);

                    const float3 radiance =
                        max(
                            g_sceneColor.SampleLevel(
                                g_sceneSampler,
                                hitUv,
                                0).rgb,
                            0.0);

                    accumulated +=
                        radiance *
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
        }
    }

    float confidence =
        saturate(validRayCount / 4.0);

    float3 indirect =
        accumulatedWeight > 1.0e-5
            ? accumulated /
                accumulatedWeight
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
        g.historyCompatible != 0u &&
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
)";

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
    uint reserved;
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

ScreenSpaceFinalGatherRenderer::
ScreenSpaceFinalGatherRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto gather =
        compiler.Compile({
            .source = kGatherCs,
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
            .pushConstantDwords = 16U,
            .shaderResourceBuffers = 0U,
            .storageTextures = 2U,
            .sampledTextures = 7U
        });

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
    const ScreenSpaceFinalGatherSettings& settings)
{
    if (width == 0U || height == 0U)
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
        std::clamp(
            settings.stepsPerRay,
            2U,
            32U),
        historyCompatible ? 1U : 0U,

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
            settings.radiusMeters,
            0.05F)),
        bits(std::max(
            settings.thicknessMeters,
            0.001F))
    };

    const std::array<u32, 4> tuning{
        bits(std::clamp(
            settings.temporalWeight,
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

    std::array<u32, 16> merged =
        constants;
    // Push constants are exactly 16 dwords. Re-purpose the final four
    // depth/tuning entries by packing tuning into the shader's gatherTuning
    // float4 through a second constants upload would overwrite from offset 0,
    // so construct the final layout directly below.
    merged[12] = tuning[0];
    merged[13] = tuning[1];
    merged[14] = tuning[2];
    merged[15] = tuning[3];

    // Radius/thickness are encoded by using the view's near/far pair and
    // deriving trace scale from settings in the same final vector is required.
    // Use a 20-dword pipeline layout instead; constants below match shader.
    const std::array<u32, 20> fullConstants{
        width,
        height,
        std::clamp(settings.stepsPerRay, 2U, 32U),
        historyCompatible ? 1U : 0U,

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
        tuning[3]
    };

    commands.SetComputePipeline(
        *gatherPipeline_);
    commands.SetComputeConstants(
        fullConstants);

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

    commands.Dispatch(
        (width + 7U) / 8U,
        (height + 7U) / 8U,
        1U);
}

void ScreenSpaceFinalGatherRenderer::Combine(
    rhi::CommandList& commands,
    rhi::Texture& sceneColor,
    rhi::Texture& indirect,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    const f32 intensity)
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
        0U
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
