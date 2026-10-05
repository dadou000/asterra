#include <orbit/lighting/ProxySurface.hpp>
#include <orbit/lighting/SurfaceBuffer.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace orbit::lighting
{
namespace
{
constexpr u32 kVerticesPerProxy = 36U;

constexpr const char* kVertexShader = R"(
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

struct Constants
{
    float4 right;             // xyz basis, w = x projection scale
    float4 up;                // xyz basis, w = y projection scale
    float4 forward;           // xyz basis, w = reverse-Z depth scale
    float4 sceneToCameraBias; // xyz = scene origin - camera, w = reverse-Z depth bias
    float4 albedoRoughness;
    float4 metadata;          // x = packed surface class + representation
};
[[vk::push_constant]] Constants g;

struct VSOutput
{
    float4 position : SV_Position;
    float3 relative : TEXCOORD0;
    nointerpolation float3 normal : TEXCOORD1;
    nointerpolation uint primitive : TEXCOORD2;
};

float4 Project(float3 p)
{
    const float3 v = float3(
        dot(p, g.right.xyz),
        dot(p, g.up.xyz),
        dot(p, g.forward.xyz));

    return float4(
        v.x * g.right.w,
        v.y * g.up.w,
        v.z * g.forward.w + g.sceneToCameraBias.w,
        v.z);
}

VSOutput main(uint vertexId : SV_VertexID)
{
    const uint primitiveIndex = vertexId / 36u;
    const uint local = vertexId % 36u;
    const uint face = local / 6u;
    const uint corner = local % 6u;

    const uint quad[6] = {0u, 1u, 2u, 2u, 1u, 3u};
    const uint q = quad[corner];
    const float2 uv =
        float2(float(q & 1u), float(q >> 1u)) * 2.0 - 1.0;

    const uint axis = face >> 1u;
    const float sgn = (face & 1u) != 0u ? 1.0 : -1.0;

    float3 unitPoint = float3(uv.x, uv.y, sgn);
    float3 faceNormal = float3(0.0, 0.0, sgn);

    if (axis == 0u)
    {
        unitPoint = float3(sgn, uv.x, uv.y);
        faceNormal = float3(sgn, 0.0, 0.0);
    }
    else if (axis == 1u)
    {
        unitPoint = float3(uv.x, sgn, uv.y);
        faceNormal = float3(0.0, sgn, 0.0);
    }

    const ProxyPrimitive prim = g_primitives[primitiveIndex];

    // A sphere is drawn as its bounding cube and ray-traced in the pixel shader.
    const bool box = prim.centerType.w >= 0.5;
    const float3 extents = box
        ? float3(prim.axisXExtent.w, prim.axisYExtent.w, prim.axisZExtent.w)
        : prim.axisXExtent.www;

    const float3 center =
        prim.centerType.xyz + g.sceneToCameraBias.xyz;

    const float3 worldPoint =
        center +
        prim.axisXExtent.xyz * (unitPoint.x * extents.x) +
        prim.axisYExtent.xyz * (unitPoint.y * extents.y) +
        prim.axisZExtent.xyz * (unitPoint.z * extents.z);

    VSOutput output;
    output.position = Project(worldPoint);
    output.relative = worldPoint;
    output.normal = normalize(
        prim.axisXExtent.xyz * faceNormal.x +
        prim.axisYExtent.xyz * faceNormal.y +
        prim.axisZExtent.xyz * faceNormal.z);
    output.primitive = primitiveIndex;
    return output;
}
)";

constexpr const char* kPixelShader = R"(
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

struct Constants
{
    float4 right;
    float4 up;
    float4 forward;
    float4 sceneToCameraBias;
    float4 albedoRoughness;
    float4 metadata;
};
[[vk::push_constant]] Constants g;

struct VSOutput
{
    float4 position : SV_Position;
    float3 relative : TEXCOORD0;
    nointerpolation float3 normal : TEXCOORD1;
    nointerpolation uint primitive : TEXCOORD2;
};

struct PSOutput
{
    float4 sceneColor : SV_Target0;
    float4 baseRoughness : SV_Target1;
    float4 normalMetallic : SV_Target2;
    float4 emissionClass : SV_Target3;
    float depth : SV_Depth;
};

float4 Project(float3 p)
{
    const float3 v = float3(
        dot(p, g.right.xyz),
        dot(p, g.up.xyz),
        dot(p, g.forward.xyz));

    return float4(
        v.x * g.right.w,
        v.y * g.up.w,
        v.z * g.forward.w + g.sceneToCameraBias.w,
        v.z);
}

float3 Albedo(uint materialId)
{
    if (materialId == 0u)
    {
        return g.albedoRoughness.rgb;
    }

    const float hue = frac(float(materialId) * 0.61803398875);
    const float3 rgb = saturate(
        abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);

    return lerp(float3(0.5, 0.5, 0.5), rgb, 0.6) * 0.7;
}

PSOutput main(VSOutput input)
{
    const ProxyPrimitive prim = g_primitives[input.primitive];
    const float3 viewDirection = normalize(input.relative);

    float3 normal = normalize(input.normal);
    float depth = input.position.z;

    if (prim.centerType.w < 0.5)
    {
        const float3 center =
            prim.centerType.xyz + g.sceneToCameraBias.xyz;
        const float radius = max(prim.axisXExtent.w, 0.0);
        const float3 toOrigin = -center;
        const float b = dot(toOrigin, viewDirection);
        const float c = dot(toOrigin, toOrigin) - radius * radius;
        const float discriminant = b * b - c;

        if (discriminant < 0.0)
        {
            discard;
        }

        const float root = sqrt(discriminant);
        float t = -b - root;

        if (t < 0.0)
        {
            t = -b + root;
        }

        if (t < 0.0)
        {
            discard;
        }

        const float3 hit = viewDirection * t;
        normal = normalize(hit - center);

        const float4 clip = Project(hit);
        depth = clip.z / max(clip.w, 1.0e-6);
    }

    // Thin walls are seen from both sides: shade the face toward the eye.
    if (dot(normal, viewDirection) > 0.0)
    {
        normal = -normal;
    }

    PSOutput output;
    output.sceneColor = float4(0.0, 0.0, 0.0, 1.0);
    output.baseRoughness =
        float4(Albedo(prim.materialId), g.albedoRoughness.w);
    output.normalMetallic = float4(normal, 0.0);
    output.emissionClass = float4(0.0, 0.0, 0.0, g.metadata.x);
    output.depth = depth;
    return output;
}
)";
} // namespace

void ProxySurfaceGeometry::Rebuild(
    rhi::Device& device,
    const SoftwareProxyScene& scene,
    const math::Double3 gpuOriginInFrameMeters)
{
    buffer_.reset();
    count_ = 0U;
    origin_ = gpuOriginInFrameMeters;
    centroid_ = {};
    radius_ = 0.0;

    const auto primitives =
        scene.GpuPrimitives(
            gpuOriginInFrameMeters);

    if (primitives.empty())
    {
        return;
    }

    buffer_ =
        device.CreateBuffer({
            .sizeBytes =
                static_cast<u64>(primitives.size()) *
                sizeof(GpuVisibilityProxyPrimitive),
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::ShaderResource
        });

    std::memcpy(
        buffer_->Map(),
        primitives.data(),
        primitives.size() *
            sizeof(GpuVisibilityProxyPrimitive));
    buffer_->Unmap();

    count_ =
        static_cast<u32>(primitives.size());

    // Bounds in the scene frame, from the centres and the largest extent of
    // each primitive (a conservative bound; it only gates a rebuild).
    math::Double3 sum{};
    for (const auto& primitive : primitives)
    {
        sum = sum + math::Double3{
            static_cast<f64>(primitive.centerType.x),
            static_cast<f64>(primitive.centerType.y),
            static_cast<f64>(primitive.centerType.z)};
    }
    const math::Double3 meanFromOrigin =
        sum / static_cast<f64>(primitives.size());
    centroid_ = origin_ + meanFromOrigin;

    for (const auto& primitive : primitives)
    {
        const math::Double3 center{
            static_cast<f64>(primitive.centerType.x),
            static_cast<f64>(primitive.centerType.y),
            static_cast<f64>(primitive.centerType.z)};
        const f64 extent =
            math::Length(math::Double3{
                static_cast<f64>(primitive.axisXExtent.w),
                static_cast<f64>(primitive.axisYExtent.w),
                static_cast<f64>(primitive.axisZExtent.w)});

        radius_ = std::max(
            radius_,
            math::Length(center - meanFromOrigin) + extent);
    }
}

bool ProxySurfaceGeometry::Ready() const noexcept
{
    return buffer_ != nullptr && count_ > 0U;
}

u32 ProxySurfaceGeometry::PrimitiveCount() const noexcept
{
    return count_;
}

rhi::Buffer* ProxySurfaceGeometry::PrimitiveBuffer() const noexcept
{
    return buffer_.get();
}

math::Double3
ProxySurfaceGeometry::GpuOriginInFrameMeters() const noexcept
{
    return origin_;
}

math::Double3
ProxySurfaceGeometry::CentroidInFrameMeters() const noexcept
{
    return centroid_;
}

f64 ProxySurfaceGeometry::BoundingRadiusMeters() const noexcept
{
    return radius_;
}

bool ProxyGpuOriginIsStale(
    const math::Double3& cameraInFrameMeters,
    const math::Double3& gpuOriginInFrameMeters,
    const math::Double3& proxyCentroidInFrameMeters,
    const f64 proxyBoundingRadiusMeters,
    const f64 driftThresholdMeters,
    const f64 relevanceMeters) noexcept
{
    const f64 drift =
        math::Length(
            cameraInFrameMeters -
            gpuOriginInFrameMeters);
    const f64 toProxies =
        math::Length(
            cameraInFrameMeters -
            proxyCentroidInFrameMeters);

    return
        std::isfinite(drift) &&
        std::isfinite(toProxies) &&
        drift > driftThresholdMeters &&
        toProxies <
            relevanceMeters +
                std::max(proxyBoundingRadiusMeters, 0.0);
}

std::array<u32, kProxySurfaceConstantDwords>
PackProxySurfaceConstants(
    const LightingView& view,
    const u32 width,
    const u32 height,
    const math::Double3& sceneToCameraMeters,
    const ProxySurfaceSettings& settings)
{
    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    // Same basis the lighting passes use to reconstruct positions from depth
    // (right = forward x up in the right-handed body frame), so a proxy lands
    // exactly where a surface at that position would reconstruct.
    const auto forward =
        math::Normalize(view.forward);
    const auto right =
        math::Normalize(
            math::Cross(
                forward,
                math::Normalize(view.up)));
    const auto up =
        math::Cross(
            right,
            forward);

    const f32 aspect =
        height > 0U
            ? static_cast<f32>(width) /
                static_cast<f32>(height)
            : 1.0F;
    const f32 yScale =
        1.0F /
        std::tan(
            std::max(view.verticalFovRadians, 1.0e-4F) *
            0.5F);
    const f32 xScale =
        yScale / std::max(aspect, 1.0e-4F);

    const f32 nearPlane =
        std::max(view.nearPlaneMeters, 1.0e-5F);
    const f32 farPlane =
        std::max(
            view.farPlaneMeters,
            nearPlane + 1.0e-4F);
    const f32 inverseRange =
        1.0F / (farPlane - nearPlane);

    const f32 metadata =
        EncodeSurfaceMetadata(
            SurfaceClass::RigidGeometry,
            SurfaceRepresentation::LocalMesh);

    return {
        bits(right.x),
        bits(right.y),
        bits(right.z),
        bits(xScale),

        bits(up.x),
        bits(up.y),
        bits(up.z),
        bits(yScale),

        bits(forward.x),
        bits(forward.y),
        bits(forward.z),
        bits(-nearPlane * inverseRange),

        bits(static_cast<f32>(sceneToCameraMeters.x)),
        bits(static_cast<f32>(sceneToCameraMeters.y)),
        bits(static_cast<f32>(sceneToCameraMeters.z)),
        bits(nearPlane * farPlane * inverseRange),

        bits(std::clamp(settings.defaultAlbedo.x, 0.0F, 1.0F)),
        bits(std::clamp(settings.defaultAlbedo.y, 0.0F, 1.0F)),
        bits(std::clamp(settings.defaultAlbedo.z, 0.0F, 1.0F)),
        bits(std::clamp(settings.roughness, 0.0F, 1.0F)),

        bits(metadata),
        0U,
        0U,
        0U
    };
}

ProxySurfaceRenderer::ProxySurfaceRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto vs =
        compiler.Compile({
            .source = kVertexShader,
            .entryPoint = "main",
            .stage = shader::Stage::Vertex,
            .debug = false
        });

    const auto ps =
        compiler.Compile({
            .source = kPixelShader,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

    pipeline_ =
        device.CreateGraphicsPipeline({
            .vertexShader = {
                .data = vs.bytecode.data(),
                .size = vs.bytecode.size()
            },
            .pixelShader = {
                .data = ps.bytecode.data(),
                .size = ps.bytecode.size()
            },
            .vertexAttributes = {},
            .vertexStrideBytes = 0U,
            .pushConstantDwords =
                kProxySurfaceConstantDwords,
            .shaderResourceBuffers = 1U,
            .topology =
                rhi::PrimitiveTopology::TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Opaque,
            .depthCompare =
                rhi::DepthCompare::GreaterEqual,
            .depthTest = true,
            .depthWrite = true,
            .colorAttachmentFormats = {
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float
            },
            .colorAttachmentCount = 4U
        });
}

void ProxySurfaceRenderer::Draw(
    rhi::CommandList& commands,
    ProxySurfaceGeometry& geometry,
    rhi::Texture& sceneColor,
    rhi::Texture& surfaceBaseRoughness,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& surfaceEmissionClass,
    rhi::Texture& depth,
    const u32 width,
    const u32 height,
    const LightingView& view,
    const ProxySurfaceSettings& settings)
{
    if (pipeline_ == nullptr ||
        !geometry.Ready() ||
        width == 0U ||
        height == 0U)
    {
        return;
    }

    const auto constants =
        PackProxySurfaceConstants(
            view,
            width,
            height,
            geometry.GpuOriginInFrameMeters() -
                view.cameraPositionInFrameMeters,
            settings);

    const std::array<rhi::Texture*, 4> targets{
        &sceneColor,
        &surfaceBaseRoughness,
        &surfaceNormalMetallic,
        &surfaceEmissionClass
    };

    commands.SetRenderTargets(
        targets,
        &depth);
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

    commands.SetGraphicsPipeline(*pipeline_);
    commands.SetGraphicsConstants(constants);
    commands.SetGraphicsBuffer(
        0U,
        *geometry.PrimitiveBuffer());

    commands.Draw(
        geometry.PrimitiveCount() *
        kVerticesPerProxy);
}
} // namespace orbit::lighting
