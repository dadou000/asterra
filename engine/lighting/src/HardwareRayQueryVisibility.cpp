#include <orbit/lighting/HardwareRayQueryVisibility.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace orbit::lighting
{
namespace
{
constexpr const char* kRayQueryCompute = R"(
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
ByteAddressBuffer g_queries : register(t0);

[[vk::binding(1, 0)]]
RWByteAddressBuffer g_results : register(u1);

[[vk::binding(2, 0)]]
StructuredBuffer<ProxyPrimitive> g_primitives : register(t2);

[[vk::binding(3, 0)]]
RaytracingAccelerationStructure g_scene : register(t3);

struct Constants
{
    uint queryCount;
    uint primitiveCount;
    uint reserved0;
    uint reserved1;
};

[[vk::push_constant]]
Constants g;

static const uint kResolutionUnresolved = 0u;
static const uint kResolutionHit = 1u;
static const uint kResolutionMiss = 2u;
static const uint kBackendHardwareRayQuery = 5u;

struct ShapeHit
{
    bool hit;
    float t;
    float3 normal;
};

ShapeHit IntersectSphere(
    float3 origin,
    float3 direction,
    ProxyPrimitive primitive,
    float minimumDistance,
    float maximumDistance)
{
    ShapeHit result;
    result.hit = false;
    result.t = 0.0;
    result.normal = 0.0;

    const float radius =
        max(primitive.axisXExtent.w, 0.0);

    const float3 offset =
        origin - primitive.centerType.xyz;

    const float b =
        dot(offset, direction);

    const float c =
        dot(offset, offset) -
        radius * radius;

    const float discriminant =
        b * b - c;

    if (discriminant < 0.0)
    {
        return result;
    }

    const float root =
        sqrt(max(discriminant, 0.0));

    float t = -b - root;

    if (t < minimumDistance ||
        t > maximumDistance)
    {
        t = -b + root;
    }

    if (t < minimumDistance ||
        t > maximumDistance)
    {
        return result;
    }

    const float3 hitPoint =
        origin + direction * t;

    result.hit = true;
    result.t = t;
    result.normal =
        normalize(hitPoint - primitive.centerType.xyz);
    return result;
}

ShapeHit IntersectBox(
    float3 origin,
    float3 direction,
    ProxyPrimitive primitive,
    float minimumDistance,
    float maximumDistance)
{
    ShapeHit result;
    result.hit = false;
    result.t = 0.0;
    result.normal = 0.0;

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

    float nearT = minimumDistance;
    float farT = maximumDistance;
    uint nearAxis = 0u;
    float nearSign = 1.0;

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
                return result;
            }
            continue;
        }

        float t0 = (-e - o) / d;
        float t1 = ( e - o) / d;
        float sign = -1.0;

        if (t0 > t1)
        {
            const float temp = t0;
            t0 = t1;
            t1 = temp;
            sign = 1.0;
        }

        if (t0 > nearT)
        {
            nearT = t0;
            nearAxis = axis;
            nearSign = sign;
        }

        farT = min(farT, t1);

        if (farT < nearT)
        {
            return result;
        }
    }

    float3 localNormal = 0.0;
    localNormal[nearAxis] = nearSign;

    result.hit = true;
    result.t = nearT;
    result.normal =
        normalize(
            primitive.axisXExtent.xyz * localNormal.x +
            primitive.axisYExtent.xyz * localNormal.y +
            primitive.axisZExtent.xyz * localNormal.z);

    return result;
}

ShapeHit IntersectPrimitive(
    float3 origin,
    float3 direction,
    ProxyPrimitive primitive,
    float minimumDistance,
    float maximumDistance)
{
    if (primitive.centerType.w >= 0.5)
    {
        return IntersectBox(
            origin,
            direction,
            primitive,
            minimumDistance,
            maximumDistance);
    }

    return IntersectSphere(
        origin,
        direction,
        primitive,
        minimumDistance,
        maximumDistance);
}

float ConfidenceForError(
    float nominalErrorMeters,
    float maximumErrorMeters)
{
    const float error =
        max(nominalErrorMeters, 0.0);

    if (isinf(maximumErrorMeters))
    {
        return 1.0 / (1.0 + error);
    }

    if (maximumErrorMeters <= 0.0)
    {
        return
            error <= 0.0
                ? 1.0
                : 0.0;
    }

    return
        saturate(
            1.0 -
            error / maximumErrorMeters);
}

void StoreResult(
    uint index,
    uint resolution,
    float confidence,
    float distanceMeters,
    float3 position,
    float3 normal,
    uint materialId,
    uint instanceId)
{
    const uint base = index * 48u;

    g_results.Store(base + 0u, resolution);
    g_results.Store(base + 4u, kBackendHardwareRayQuery);
    g_results.Store(base + 8u, asuint(saturate(confidence)));
    g_results.Store(base + 12u, asuint(max(distanceMeters, 0.0)));

    g_results.Store3(base + 16u, asuint(position));
    g_results.Store(base + 28u, materialId);

    g_results.Store3(base + 32u, asuint(normal));
    g_results.Store(base + 44u, instanceId);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint index = dispatchId.x;

    if (index >= g.queryCount)
    {
        return;
    }

    const uint queryBase =
        index * 48u;

    const float4 originMinimum =
        asfloat(
            g_queries.Load4(
                queryBase + 0u));

    const float4 directionMaximum =
        asfloat(
            g_queries.Load4(
                queryBase + 16u));

    const float4 requirements =
        asfloat(
            g_queries.Load4(
                queryBase + 32u));

    const float3 origin =
        originMinimum.xyz;

    const float3 direction =
        normalize(directionMaximum.xyz);

    const float minimumDistance =
        max(originMinimum.w, 0.0);

    const float maximumDistance =
        max(directionMaximum.w, minimumDistance);

    const float maximumErrorMeters =
        requirements.x;

    bool rejectedForLod = false;

    RayQuery<RAY_FLAG_NONE> query;

    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = minimumDistance;
    ray.Direction = direction;
    ray.TMax = maximumDistance;

    query.TraceRayInline(
        g_scene,
        RAY_FLAG_NONE,
        0xFF,
        ray);

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

        const ProxyPrimitive primitive =
            g_primitives[primitiveIndex];

        if (!isinf(maximumErrorMeters) &&
            primitive.nominalErrorMeters >
                maximumErrorMeters)
        {
            rejectedForLod = true;
            continue;
        }

        const ShapeHit hit =
            IntersectPrimitive(
                origin,
                direction,
                primitive,
                minimumDistance,
                maximumDistance);

        if (hit.hit)
        {
            query.CommitProceduralPrimitiveHit(
                hit.t);
        }
    }

    if (query.CommittedStatus() ==
        COMMITTED_PROCEDURAL_PRIMITIVE_HIT)
    {
        const uint primitiveIndex =
            query.CommittedPrimitiveIndex();

        if (primitiveIndex >= g.primitiveCount)
        {
            StoreResult(
                index,
                kResolutionUnresolved,
                0.0,
                0.0,
                0.0,
                0.0,
                0u,
                0u);
            return;
        }

        const ProxyPrimitive primitive =
            g_primitives[primitiveIndex];

        const ShapeHit hit =
            IntersectPrimitive(
                origin,
                direction,
                primitive,
                minimumDistance,
                maximumDistance);

        if (!hit.hit)
        {
            StoreResult(
                index,
                kResolutionUnresolved,
                0.0,
                0.0,
                0.0,
                0.0,
                0u,
                0u);
            return;
        }

        const float confidence =
            ConfidenceForError(
                primitive.nominalErrorMeters,
                maximumErrorMeters);

        StoreResult(
            index,
            kResolutionHit,
            confidence,
            hit.t,
            origin + direction * hit.t,
            hit.normal,
            primitive.materialId,
            primitive.instanceId);
        return;
    }

    StoreResult(
        index,
        rejectedForLod
            ? kResolutionUnresolved
            : kResolutionMiss,
        rejectedForLod ? 0.0 : 1.0,
        0.0,
        0.0,
        0.0,
        0u,
        0u);
}
)";
} // namespace

HardwareRayQueryVisibilityBatch::
HardwareRayQueryVisibilityBatch(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : device_(&device),
      supported_(
          device.Capabilities().
              accelerationStructures &&
          device.Capabilities().
              rayQuery)
{
    if (!supported_)
    {
        return;
    }

    const auto compute =
        compiler.Compile({
            .source = kRayQueryCompute,
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
                .data =
                    compute.bytecode.data(),
                .size =
                    compute.bytecode.size()
            },
            .pushConstantDwords = 4U,
            .shaderResourceBuffers = 3U,
            .storageTextures = 0U,
            .sampledTextures = 0U,
            .accelerationStructures = 1U
        });
}

void HardwareRayQueryVisibilityBatch::RebuildScene(
    const SoftwareProxyScene& scene,
    const math::Double3 gpuOriginInFrameMeters)
{
    accelerationStructure_.reset();
    primitiveBuffer_.reset();
    primitiveCount_ = 0U;
    sceneFrame_ = scene.Frame();
    gpuOriginInFrameMeters_ =
        gpuOriginInFrameMeters;

    if (!supported_ ||
        device_ == nullptr ||
        scene.Stats().proxyCount == 0U)
    {
        return;
    }

    const auto aabbs =
        scene.AccelerationAabbs(
            gpuOriginInFrameMeters);

    const auto primitives =
        scene.GpuPrimitives(
            gpuOriginInFrameMeters);

    if (aabbs.empty() ||
        aabbs.size() != primitives.size())
    {
        throw std::logic_error(
            "Orbit hardware visibility proxy exports are inconsistent.");
    }

    accelerationStructure_ =
        device_->CreateAabbAccelerationStructure(
            aabbs);

    primitiveBuffer_ =
        device_->CreateBuffer({
            .sizeBytes =
                static_cast<u64>(
                    primitives.size()) *
                sizeof(
                    GpuVisibilityProxyPrimitive),
            .usage =
                rhi::BufferUsage::Structured,
            .memory =
                rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::ShaderResource
        });

    auto* destination =
        primitiveBuffer_->Map();

    std::memcpy(
        destination,
        primitives.data(),
        primitives.size() *
            sizeof(
                GpuVisibilityProxyPrimitive));

    primitiveBuffer_->Unmap();

    primitiveCount_ =
        static_cast<u32>(
            primitives.size());
}

bool HardwareRayQueryVisibilityBatch::Supported() const noexcept
{
    return supported_;
}

bool HardwareRayQueryVisibilityBatch::Ready() const noexcept
{
    return
        supported_ &&
        pipeline_ != nullptr &&
        accelerationStructure_ != nullptr &&
        primitiveBuffer_ != nullptr &&
        primitiveCount_ > 0U;
}

u32 HardwareRayQueryVisibilityBatch::PrimitiveCount() const noexcept
{
    return primitiveCount_;
}

frames::FrameId
HardwareRayQueryVisibilityBatch::SceneFrame() const noexcept
{
    return sceneFrame_;
}

math::Double3
HardwareRayQueryVisibilityBatch::GpuOriginInFrameMeters() const noexcept
{
    return gpuOriginInFrameMeters_;
}

GpuVisibilityQuery
HardwareRayQueryVisibilityBatch::EncodeQuery(
    const VisibilityQuery& query) const
{
    if (!sceneFrame_ ||
        query.frame != sceneFrame_)
    {
        throw std::invalid_argument(
            "Orbit hardware visibility query frame does not match the "
            "acceleration-structure scene frame.");
    }

    LightingView encodingView;
    encodingView.frame =
        sceneFrame_;
    encodingView.gpuOriginInFrameMeters =
        gpuOriginInFrameMeters_;

    return
        EncodeGpuVisibilityQuery(
            query,
            encodingView);
}

void HardwareRayQueryVisibilityBatch::Dispatch(
    rhi::CommandList& commands,
    rhi::Buffer& queries,
    rhi::Buffer& results,
    const u32 queryCount)
{
    if (queryCount == 0U)
    {
        return;
    }

    if (!Ready())
    {
        throw std::logic_error(
            "Orbit hardware ray-query visibility is not ready.");
    }

    const std::array<u32, 4> constants{
        queryCount,
        primitiveCount_,
        0U,
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
    commands.SetComputeBuffer(
        2U,
        *primitiveBuffer_);

    commands.SetComputeAccelerationStructure(
        0U,
        *accelerationStructure_);

    commands.Dispatch(
        (queryCount + 63U) / 64U,
        1U,
        1U);
}
} // namespace orbit::lighting
