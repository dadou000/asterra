#include <orbit/lighting/ProxySunShadow.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace orbit::lighting
{
namespace
{
constexpr const char* kSunShadowCompute = R"(
struct ProxyPrimitive
{
    float4 centerType;
    float4 axisXExtent;
    float4 axisYExtent;
    float4 axisZExtent;
    uint materialId;
    uint instanceId;
    float nominalErrorMeters;
    uint reserved;
};

[[vk::binding(0, 0)]]
StructuredBuffer<ProxyPrimitive> g_primitives : register(t0);

[[vk::binding(1, 0)]]
RWTexture2D<float4> g_shadow : register(u1);

[[vk::binding(2, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_normalMetallic : register(t2);
[[vk::binding(2, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_normalSampler : register(s2);

[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth : register(t3);
[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler : register(s3);

[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_emissionClass : register(t4);
[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_emissionSampler : register(s4);

[[vk::binding(5, 0)]]
RaytracingAccelerationStructure g_scene : register(t5);

struct Constants
{
    uint width;
    uint height;
    uint primitiveCount;
    float maximumDistance;

    float4 forwardAspect;
    float4 upTanHalfFov;
    float4 depthRangeSkyRays; // x near, y far, z sky ray count (as a float)
    float4 cameraToScene;
    float4 sunDirectionBias;
    float4 skyIrradianceDistance; // rgb sky irradiance, w sky ray distance
    float4 bodyUp;                // xyz local up (radial) direction
};

[[vk::push_constant]]
Constants g;

float ReverseZViewDepth(float depth)
{
    const float nearPlane =
        max(g.depthRangeSkyRays.x, 1.0e-5);
    const float farPlane =
        max(g.depthRangeSkyRays.y, nearPlane + 1.0e-4);

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

    const float rayForward =
        max(dot(ray, forward), 1.0e-5);

    return
        ray *
        (ReverseZViewDepth(depth) / rayForward);
}

// Any intersection within [0, maximumDistance] occludes the sun. An origin
// inside a solid proxy counts as occluded (the slab test starts at t = 0).
bool HitsPrimitive(
    float3 origin,
    float3 direction,
    ProxyPrimitive primitive,
    float maximumDistance,
    out float hitDistance)
{
    hitDistance = 0.0;

    if (primitive.centerType.w < 0.5)
    {
        const float radius =
            max(primitive.axisXExtent.w, 0.0);
        const float3 offset =
            origin - primitive.centerType.xyz;
        const float b = dot(offset, direction);
        const float c =
            dot(offset, offset) - radius * radius;
        const float discriminant = b * b - c;

        if (discriminant < 0.0)
        {
            return false;
        }

        const float root = sqrt(discriminant);
        float t = -b - root;

        if (t < 0.0)
        {
            t = -b + root;
        }

        hitDistance = max(t, 0.0);
        return t >= 0.0 && t <= maximumDistance;
    }

    const float3 relative =
        origin - primitive.centerType.xyz;
    const float3 localOrigin = {
        dot(relative, primitive.axisXExtent.xyz),
        dot(relative, primitive.axisYExtent.xyz),
        dot(relative, primitive.axisZExtent.xyz)
    };
    const float3 localDirection = {
        dot(direction, primitive.axisXExtent.xyz),
        dot(direction, primitive.axisYExtent.xyz),
        dot(direction, primitive.axisZExtent.xyz)
    };
    const float3 extents = {
        max(primitive.axisXExtent.w, 0.0),
        max(primitive.axisYExtent.w, 0.0),
        max(primitive.axisZExtent.w, 0.0)
    };

    float nearT = 0.0;
    float farT = maximumDistance;

    [unroll]
    for (uint axis = 0u; axis < 3u; ++axis)
    {
        const float o = localOrigin[axis];
        const float d = localDirection[axis];
        const float e = extents[axis];

        if (abs(d) <= 1.0e-7)
        {
            if (o < -e || o > e)
            {
                return false;
            }
            continue;
        }

        float t0 = (-e - o) / d;
        float t1 = ( e - o) / d;

        if (t0 > t1)
        {
            const float temp = t0;
            t0 = t1;
            t1 = temp;
        }

        nearT = max(nearT, t0);
        farT = min(farT, t1);

        if (farT < nearT)
        {
            return false;
        }
    }

    hitDistance = nearT;
    return true;
}

bool Occluded(
    float3 origin,
    float3 direction,
    float maximumDistance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = 0.0;
    ray.Direction = direction;
    ray.TMax = maximumDistance;

    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(
        g_scene,
        RAY_FLAG_NONE,
        0xFF,
        ray);

    bool occluded = false;

    while (query.Proceed())
    {
        if (query.CandidateType() !=
            CANDIDATE_PROCEDURAL_PRIMITIVE)
        {
            continue;
        }

        const uint primitiveIndex =
            query.CandidatePrimitiveIndex();

        if (primitiveIndex >= g.primitiveCount)
        {
            continue;
        }

        float hitDistance;

        if (HitsPrimitive(
                origin,
                direction,
                g_primitives[primitiveIndex],
                maximumDistance,
                hitDistance))
        {
            occluded = true;
            query.CommitProceduralPrimitiveHit(
                hitDistance);
            query.Abort();
        }
    }

    return occluded;
}

uint Pcg(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word =
        ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Integer hash: a sin()-based one loses accuracy for large arguments on the
// GPU and correlates whole pixel rows, which shows as banding.
float Hash12(float2 p)
{
    const uint2 q = uint2(p);
    return float(Pcg(q.x + Pcg(q.y))) * (1.0 / 4294967296.0);
}

// Cosine-weighted direction about `normal`; (u, v) in [0, 1).
float3 CosineDirection(float3 normal, float u, float v)
{
    const float3 helper =
        abs(normal.y) < 0.99
            ? float3(0.0, 1.0, 0.0)
            : float3(1.0, 0.0, 0.0);
    const float3 tangent =
        normalize(cross(helper, normal));
    const float3 bitangent =
        cross(normal, tangent);

    const float radius = sqrt(u);
    const float phi = 6.28318530718 * v;

    return normalize(
        tangent * (radius * cos(phi)) +
        bitangent * (radius * sin(phi)) +
        normal * sqrt(max(1.0 - u, 0.0)));
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

    float sunVisibility = 1.0;
    float3 skyIrradiance = 0.0;

    if (depth > 0.0)
    {
        const float3 normal =
            normalize(
                g_normalMetallic.SampleLevel(
                    g_normalSampler,
                    uv,
                    0).xyz);

        const float3 toSun =
            normalize(g.sunDirectionBias.xyz);

        const float3 position =
            ReconstructPosition(uv, depth);

        const float bias =
            g.sunDirectionBias.w +
            ReverseZViewDepth(depth) * 2.0e-4;

        const float3 origin =
            position +
            g.cameraToScene.xyz +
            normal * bias;

        // Surfaces facing away from the sun take no direct light anyway.
        if (dot(normal, toSun) > 0.0)
        {
            if (Occluded(origin, toSun, g.maximumDistance))
            {
                sunVisibility = 0.0;
            }
        }

        // Sky fill for authored proxy surfaces only (surface class 3): the
        // fraction of the cosine-weighted hemisphere that is both above the
        // local horizon and left open by the other proxies, times the
        // atmosphere's sky irradiance (which is the irradiance on an up-facing
        // surface). Terrain is not in this scene, so a direction below the
        // horizon would otherwise read as open sky and light the underside of
        // a roof. Terrain keeps the lighting it already had.
        const float surfaceClass =
            floor(
                g_emissionClass.SampleLevel(
                    g_emissionSampler,
                    uv,
                    0).a + 0.01);

        const uint rayCount =
            min(uint(g.depthRangeSkyRays.z + 0.5), 64u);

        if (surfaceClass == 3.0 && rayCount > 0u)
        {
            const float jitterU =
                Hash12(float2(pixel));
            const float jitterV =
                Hash12(float2(pixel) + float2(17.0, 41.0));

            const float3 up =
                normalize(g.bodyUp.xyz);

            float open = 0.0;

            for (uint i = 0u; i < rayCount; ++i)
            {
                const float u =
                    frac(jitterU + float(i) * 0.7548776662);
                const float v =
                    frac(jitterV + float(i) * 0.5698402910);

                const float3 direction =
                    CosineDirection(normal, u, v);

                if (dot(direction, up) > 0.0 &&
                    !Occluded(
                        origin,
                        direction,
                        g.skyIrradianceDistance.w))
                {
                    open += 1.0;
                }
            }

            skyIrradiance =
                g.skyIrradianceDistance.rgb *
                (open / float(rayCount));
        }
    }

    g_shadow[pixel] =
        float4(sunVisibility, skyIrradiance);
}
)";
} // namespace

std::array<u32, kProxySunShadowConstantDwords>
PackProxySunShadowConstants(
    const LightingView& view,
    const u32 width,
    const u32 height,
    const u32 primitiveCount,
    const math::Float3& directionToLight,
    const math::Double3& cameraToSceneOriginMeters,
    const ProxySunShadowSettings& settings)
{
    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    // Radial direction at the camera; straight up the Y axis if the camera
    // sits at the body origin (no meaningful radial direction).
    math::Float3 bodyUp{0.0F, 1.0F, 0.0F};
    {
        const f64 length =
            math::Length(
                view.cameraPositionInFrameMeters);

        if (std::isfinite(length) &&
            length > 1.0e-6)
        {
            const auto direction =
                view.cameraPositionInFrameMeters /
                length;
            bodyUp = {
                static_cast<f32>(direction.x),
                static_cast<f32>(direction.y),
                static_cast<f32>(direction.z)
            };
        }
    }

    const f32 aspect =
        height > 0U
            ? static_cast<f32>(width) /
                static_cast<f32>(height)
            : 1.0F;

    return {
        width,
        height,
        primitiveCount,
        bits(std::max(
            settings.maximumDistanceMeters,
            0.0F)),

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
        bits(static_cast<f32>(std::min(settings.skyRayCount, 64U))),
        0U,

        bits(static_cast<f32>(
            cameraToSceneOriginMeters.x)),
        bits(static_cast<f32>(
            cameraToSceneOriginMeters.y)),
        bits(static_cast<f32>(
            cameraToSceneOriginMeters.z)),
        0U,

        bits(directionToLight.x),
        bits(directionToLight.y),
        bits(directionToLight.z),
        bits(std::max(
            settings.normalBiasMeters,
            0.0F)),

        bits(std::max(settings.skyIrradiance.x, 0.0F)),
        bits(std::max(settings.skyIrradiance.y, 0.0F)),
        bits(std::max(settings.skyIrradiance.z, 0.0F)),
        bits(std::max(
            settings.skyMaximumDistanceMeters,
            0.0F)),

        bits(bodyUp.x),
        bits(bodyUp.y),
        bits(bodyUp.z),
        0U
    };
}

ProxySunShadowRenderer::ProxySunShadowRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    if (!device.Capabilities().accelerationStructures ||
        !device.Capabilities().rayQuery)
    {
        return;
    }

    const auto compute =
        compiler.Compile({
            .source = kSunShadowCompute,
            .entryPoint = "main",
            .stage = shader::Stage::Compute,
            .shaderModelMajor = 6U,
            .shaderModelMinor = 5U,
            .enableSpirvRayQuery = true,
            .debug = false
        });

    pipeline_ =
        device.CreateComputePipeline({
            .computeShader = {
                .data = compute.bytecode.data(),
                .size = compute.bytecode.size()
            },
            .pushConstantDwords =
                kProxySunShadowConstantDwords,
            .shaderResourceBuffers = 1U,
            .storageTextures = 1U,
            .sampledTextures = 3U,
            .accelerationStructures = 1U
        });
}

bool ProxySunShadowRenderer::Supported() const noexcept
{
    return pipeline_ != nullptr;
}

void ProxySunShadowRenderer::Draw(
    rhi::CommandList& commands,
    HardwareRayQueryVisibilityBatch& scene,
    rhi::Texture& target,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& surfaceEmissionClass,
    rhi::Texture& depth,
    const u32 width,
    const u32 height,
    const LightingView& view,
    const math::Float3& directionToLight,
    const ProxySunShadowSettings& settings)
{
    if (pipeline_ == nullptr ||
        width == 0U ||
        height == 0U ||
        !scene.Ready())
    {
        return;
    }

    const auto constants =
        PackProxySunShadowConstants(
            view,
            width,
            height,
            scene.PrimitiveCount(),
            directionToLight,
            view.cameraPositionInFrameMeters -
                scene.GpuOriginInFrameMeters(),
            settings);

    commands.SetComputePipeline(
        *pipeline_);
    commands.SetComputeConstants(
        constants);

    commands.SetComputeBuffer(
        0U,
        *scene.PrimitiveBuffer());
    commands.SetComputeStorageTexture(
        0U,
        target);
    commands.SetComputeTexture(
        0U,
        surfaceNormalMetallic);
    commands.SetComputeTexture(
        1U,
        depth);
    commands.SetComputeTexture(
        2U,
        surfaceEmissionClass);
    commands.SetComputeAccelerationStructure(
        0U,
        *scene.SceneAccelerationStructure());

    commands.Dispatch(
        (width + 7U) / 8U,
        (height + 7U) / 8U,
        1U);
}
} // namespace orbit::lighting
