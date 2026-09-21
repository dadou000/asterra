#include <orbit/lighting/ScreenSpaceVisibility.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace orbit::lighting
{
namespace
{
constexpr const char* kScreenTraceCompute = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_queries : register(t0);

[[vk::binding(1, 0)]]
RWByteAddressBuffer g_results : register(u1);

[[vk::binding(2, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth : register(t2);
[[vk::binding(2, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler : register(s2);

[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_normalMetallic : register(t3);
[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_normalSampler : register(s3);

struct PushConstants
{
    uint queryCount;
    uint viewportWidth;
    uint viewportHeight;
    uint maximumSteps;

    float4 forwardAspect;
    float4 upTanHalfFov;
    float4 depthAndTrace;
    float4 traceTuning;
};

[[vk::push_constant]]
PushConstants g;

static const uint kResolutionUnresolved = 0u;
static const uint kResolutionHit = 1u;
static const uint kResolutionMiss = 2u;
static const uint kBackendScreenSpace = 1u;

float ReverseZViewDepth(float depth)
{
    const float nearPlane =
        max(g.depthAndTrace.x, 1.0e-5);
    const float farPlane =
        max(g.depthAndTrace.y, nearPlane + 1.0e-4);

    return
        nearPlane * farPlane /
        max(
            depth * (farPlane - nearPlane) +
                nearPlane,
            1.0e-6);
}

void StoreResult(
    uint index,
    uint resolution,
    float confidence,
    float distanceMeters,
    float3 position,
    float3 normal)
{
    const uint base = index * 48u;

    g_results.Store(base + 0u, resolution);
    g_results.Store(base + 4u, kBackendScreenSpace);
    g_results.Store(base + 8u, asuint(saturate(confidence)));
    g_results.Store(base + 12u, asuint(max(distanceMeters, 0.0)));

    g_results.Store3(base + 16u, asuint(position));
    g_results.Store(base + 28u, 0u);

    g_results.Store3(base + 32u, asuint(normal));
    g_results.Store(base + 44u, 0u);
}

bool ProjectPoint(
    float3 position,
    float3 forward,
    float3 right,
    float3 up,
    float aspect,
    float tanHalfFov,
    out float2 uv,
    out float viewDepth)
{
    viewDepth = dot(position, forward);

    if (viewDepth <=
        max(g.depthAndTrace.x, 1.0e-5))
    {
        uv = 0.0;
        return false;
    }

    const float x = dot(position, right);
    const float y = dot(position, up);

    const float2 ndc = {
        x /
            (viewDepth *
             tanHalfFov *
             aspect),
        y /
            (viewDepth *
             tanHalfFov)
    };

    uv = {
        ndc.x * 0.5 + 0.5,
        0.5 - ndc.y * 0.5
    };

    return
        all(uv >= 0.0) &&
        all(uv <= 1.0);
}

float EdgeConfidence(float2 uv)
{
    const float margin =
        min(
            min(uv.x, 1.0 - uv.x),
            min(uv.y, 1.0 - uv.y));

    return
        saturate(
            margin /
            max(g.traceTuning.z, 1.0e-5));
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint index = dispatchId.x;

    if (index >= g.queryCount)
    {
        return;
    }

    const uint queryBase = index * 32u;

    const float4 originMin =
        asfloat(
            g_queries.Load4(
                queryBase + 0u));

    const float4 directionMax =
        asfloat(
            g_queries.Load4(
                queryBase + 16u));

    const float3 origin =
        originMin.xyz;

    const float3 direction =
        normalize(directionMax.xyz);

    const float minimumDistance =
        max(originMin.w, 0.0);

    const float maximumDistance =
        max(directionMax.w, minimumDistance);

    const float3 forward =
        normalize(g.forwardAspect.xyz);

    const float3 requestedUp =
        normalize(g.upTanHalfFov.xyz);

    const float3 right =
        normalize(
            cross(
                forward,
                requestedUp));

    const float3 up =
        normalize(
            cross(
                right,
                forward));

    const float aspect =
        max(g.forwardAspect.w, 0.001);

    const float tanHalfFov =
        max(g.upTanHalfFov.w, 0.001);

    float distanceMeters =
        minimumDistance;

    float previousDelta =
        -1.0e30;

    float coverageConfidence =
        1.0;

    bool hadValidDepth = false;

    [loop]
    for (uint step = 0u;
         step < g.maximumSteps &&
         distanceMeters <= maximumDistance;
         ++step)
    {
        const float3 position =
            origin +
            direction *
                distanceMeters;

        float2 uv;
        float queryViewDepth;

        if (!ProjectPoint(
                position,
                forward,
                right,
                up,
                aspect,
                tanHalfFov,
                uv,
                queryViewDepth))
        {
            StoreResult(
                index,
                kResolutionUnresolved,
                coverageConfidence * 0.5,
                0.0,
                0.0,
                0.0);
            return;
        }

        coverageConfidence =
            min(
                coverageConfidence,
                EdgeConfidence(uv));

        const float sceneDepthSample =
            g_depth.SampleLevel(
                g_depthSampler,
                uv,
                0).r;

        if (sceneDepthSample <= 0.0)
        {
            StoreResult(
                index,
                kResolutionUnresolved,
                coverageConfidence * 0.6,
                0.0,
                0.0,
                0.0);
            return;
        }

        hadValidDepth = true;

        const float sceneViewDepth =
            ReverseZViewDepth(
                sceneDepthSample);

        const float delta =
            queryViewDepth -
            sceneViewDepth;

        const float thickness =
            max(
                g.traceTuning.y,
                0.001);

        const bool crossed =
            delta >= -thickness &&
            previousDelta < -thickness;

        if (crossed)
        {
            const float4 sampledNormal =
                g_normalMetallic.SampleLevel(
                    g_normalSampler,
                    uv,
                    0);

            const float normalLength =
                length(sampledNormal.xyz);

            if (normalLength <= 0.25)
            {
                StoreResult(
                    index,
                    kResolutionUnresolved,
                    coverageConfidence * 0.5,
                    0.0,
                    0.0,
                    0.0);
                return;
            }

            const float depthAgreement =
                saturate(
                    1.0 -
                    abs(delta) /
                        (thickness * 2.0));

            const float travel =
                saturate(
                    1.0 -
                    distanceMeters /
                        max(
                            maximumDistance,
                            1.0e-4));

            const float confidence =
                coverageConfidence *
                lerp(0.75, 1.0, depthAgreement) *
                lerp(0.85, 1.0, travel);

            StoreResult(
                index,
                kResolutionHit,
                confidence,
                distanceMeters,
                position,
                normalize(sampledNormal.xyz));
            return;
        }

        previousDelta = delta;

        const float minimumStep =
            max(
                g.traceTuning.x,
                0.001);

        const float adaptiveStep =
            max(
                minimumStep,
                abs(delta) * 0.5);

        distanceMeters +=
            min(
                adaptiveStep,
                max(
                    g.depthAndTrace.z,
                    minimumStep));
    }

    StoreResult(
        index,
        hadValidDepth
            ? kResolutionMiss
            : kResolutionUnresolved,
        hadValidDepth
            ? coverageConfidence
            : 0.0,
        0.0,
        0.0,
        0.0);
}
)";
} // namespace

ScreenSpaceVisibilityBatch::
ScreenSpaceVisibilityBatch(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto compute =
        compiler.Compile({
            .source =
                kScreenTraceCompute,
            .entryPoint = "main",
            .stage =
                shader::Stage::Compute,
            .debug = false
        });

    pipeline_ =
        device.CreateComputePipeline({
            .computeShader = {
                .data =
                    compute.bytecode.data(),
                .size =
                    compute.bytecode.size()
            },
            .pushConstantDwords = 20U,
            .shaderResourceBuffers = 2U,
            .sampledTextures = 2U
        });
}

void ScreenSpaceVisibilityBatch::Dispatch(
    rhi::CommandList& commands,
    rhi::Buffer& queries,
    rhi::Buffer& results,
    rhi::Texture& depth,
    rhi::Texture& surfaceNormalMetallic,
    const u32 queryCount,
    const u32 viewportWidth,
    const u32 viewportHeight,
    const LightingView& view,
    const ScreenSpaceVisibilityConfig& config)
{
    if (queryCount == 0U ||
        viewportWidth == 0U ||
        viewportHeight == 0U)
    {
        return;
    }

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(
                value);
        };

    const u32 maximumSteps =
        std::max(
            config.maximumSteps,
            1U);

    const f32 aspect =
        static_cast<f32>(
            viewportWidth) /
        static_cast<f32>(
            viewportHeight);

    const std::array<u32, 20> constants{
        queryCount,
        viewportWidth,
        viewportHeight,
        maximumSteps,

        bits(view.forward.x),
        bits(view.forward.y),
        bits(view.forward.z),
        bits(aspect),

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
            config.maximumStepMeters,
            config.minimumStepMeters)),
        0U,

        bits(std::max(
            config.minimumStepMeters,
            0.001F)),
        bits(std::max(
            config.thicknessMeters,
            0.001F)),
        bits(std::max(
            config.edgeFadeUv,
            0.001F)),
        0U
    };

    commands.SetComputePipeline(
        *pipeline_);

    commands.SetComputeConstants(
        constants);

    commands.SetComputeBuffer(
        0U,
        queries);

    commands.SetComputeBuffer(
        1U,
        results);

    commands.SetComputeTexture(
        0U,
        depth);

    commands.SetComputeTexture(
        1U,
        surfaceNormalMetallic);

    commands.Dispatch(
        (queryCount + 63U) / 64U,
        1U,
        1U);
}
} // namespace orbit::lighting
