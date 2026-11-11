#include <orbit/mesh_render/EmissiveLights.hpp>

#include <orbit/lighting/SdfTraceShader.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

namespace orbit::mesh_render
{
namespace
{
constexpr u64 kBufferRetireTicks = 24U;

// Buffers: 0 = lights (element 0 = count, then two float4 per light: position
// + equivalent radius, radiance + bounding radius), 1..4 = the distance field.
// Storage image 0 = the lighting target. Sampled: base colour / roughness,
// normal / metallic, emission class, depth.
constexpr const char* kLightCs = R"(
[[vk::binding(0, 0)]]
StructuredBuffer<float4> g_lights : register(t0);

#define SDF_DIST_CORNERS 1
[[vk::binding(1, 0)]] RWStructuredBuffer<uint4> g_sdfDist : register(u20);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> g_sdfAlbedo : register(u21);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> g_sdfNormal : register(u22);
[[vk::binding(4, 0)]] RWStructuredBuffer<float4> g_sdfRadiance : register(u23);

[[vk::binding(5, 0)]] RWTexture2D<float4> g_target : register(u1);

[[vk::binding(6, 0)]] [[vk::combinedImageSampler]] Texture2D g_baseRoughness : register(t4);
[[vk::binding(6, 0)]] [[vk::combinedImageSampler]] SamplerState g_baseSampler : register(s4);
[[vk::binding(7, 0)]] [[vk::combinedImageSampler]] Texture2D g_normalMetallic : register(t5);
[[vk::binding(7, 0)]] [[vk::combinedImageSampler]] SamplerState g_normalSampler : register(s5);
[[vk::binding(8, 0)]] [[vk::combinedImageSampler]] Texture2D g_emissionClass : register(t6);
[[vk::binding(8, 0)]] [[vk::combinedImageSampler]] SamplerState g_emissionSampler : register(s6);
[[vk::binding(9, 0)]] [[vk::combinedImageSampler]] Texture2D g_depth : register(t7);
[[vk::binding(9, 0)]] [[vk::combinedImageSampler]] SamplerState g_depthSampler : register(s7);

struct Constants
{
    uint width;
    uint height;
    uint pad0;
    uint pad1;

    float4 forwardAspect;
    float4 upTanHalfFov;
    float4 depthRange;
    float4 sdfOriginVoxel;
    float4 sdfDimensionsEnable;
};
[[vk::push_constant]] Constants g;

#define SDF_ORIGIN g.sdfOriginVoxel.xyz
#define SDF_VOXEL g.sdfOriginVoxel.w
#define SDF_DIMS int3(g.sdfDimensionsEnable.xyz)
//SDF_TRACE_INCLUDE

float ReverseZViewDepth(float depth)
{
    const float nearPlane = max(g.depthRange.x, 1.0e-5);
    const float farPlane = max(g.depthRange.y, nearPlane + 1.0e-4);
    return nearPlane * farPlane /
        max(depth * (farPlane - nearPlane) + nearPlane, 1.0e-6);
}

float3 ReconstructPosition(float2 uv, float depth)
{
    const float3 forward = normalize(g.forwardAspect.xyz);
    const float3 right = normalize(cross(forward, normalize(g.upTanHalfFov.xyz)));
    const float3 up = normalize(cross(right, forward));
    const float aspect = max(g.forwardAspect.w, 0.001);
    const float tanHalf = max(g.upTanHalfFov.w, 0.001);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float3 ray = normalize(forward + right * (ndc.x * aspect * tanHalf) + up * (ndc.y * tanHalf));
    const float viewDepth = ReverseZViewDepth(depth);
    return ray * (viewDepth / max(dot(ray, forward), 1.0e-5));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g.width || id.y >= g.height)
    {
        return;
    }
    const uint2 pixel = id.xy;
    const float2 uv = (float2(pixel) + 0.5) / float2(g.width, g.height);

    const float4 emissionClass = g_emissionClass.SampleLevel(g_emissionSampler, uv, 0);
    const float depth = g_depth.SampleLevel(g_depthSampler, uv, 0).r;
    if (emissionClass.a <= 0.0 || depth <= 0.0)
    {
        g_target[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    const float4 baseRoughness = g_baseRoughness.SampleLevel(g_baseSampler, uv, 0);
    const float4 normalMetallic = g_normalMetallic.SampleLevel(g_normalSampler, uv, 0);
    const float3 normal = normalize(normalMetallic.xyz);
    const float3 albedo = max(baseRoughness.rgb, 0.0) * (1.0 - saturate(normalMetallic.w));
    const float3 position = ReconstructPosition(uv, depth);

    const uint count = (uint)g_lights[0].x;
    float3 irradiance = float3(0.0, 0.0, 0.0);

    [loop]
    for (uint i = 0u; i < count; ++i)
    {
        const float4 a = g_lights[1u + 2u * i];
        const float4 b = g_lights[2u + 2u * i];
        const float3 toLight = a.xyz - position;
        const float distance2 = dot(toLight, toLight);
        const float lightDistance = sqrt(distance2);
        if (lightDistance <= b.w * 1.02)
        {
            continue; // On or inside the emitter itself.
        }
        const float3 l = toLight / lightDistance;

        // Lambertian sphere of radius r: E = pi L sin^2(alpha) cos(theta), with
        // the cosine wrapped so a sphere partly above the horizon still lights.
        const float sinAlpha = min(a.w / lightDistance, 0.999);
        const float cosine = saturate((dot(normal, l) + sinAlpha) / (1.0 + sinAlpha));
        if (cosine <= 0.0)
        {
            continue;
        }
        float visibility = 1.0;
        if (g.sdfDimensionsEnable.w > 0.5)
        {
            const float3 origin = position + normal * (0.6 * SDF_VOXEL);
            const float reach = max(lightDistance - b.w * 1.05, 0.0);
            visibility = SdfSoftShadow(
                origin, l, reach, clamp(lightDistance / max(a.w, 0.02), 2.0, 64.0));
        }
        irradiance += b.rgb * (3.14159265 * sinAlpha * sinAlpha * cosine * visibility);
    }

    g_target[pixel] = float4(albedo * (1.0 / 3.14159265) * irradiance, 0.0);
}
)";

constexpr const char* kCompositeVs = R"(
float4 main(uint vertexId : SV_VertexID) : SV_Position
{
    const float2 p[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    return float4(p[vertexId], 0.0, 1.0);
}
)";

constexpr const char* kCompositePs = R"(
[[vk::binding(0, 0)]] [[vk::combinedImageSampler]] Texture2D g_lighting;
[[vk::binding(0, 0)]] [[vk::combinedImageSampler]] SamplerState g_lightingSampler;

float4 main(float4 position : SV_Position) : SV_Target0
{
    return float4(g_lighting.Load(int3(int2(position.xy), 0)).rgb, 0.0);
}
)";

[[nodiscard]] std::string LightSource()
{
    std::string source = kLightCs;
    const std::string marker = "//SDF_TRACE_INCLUDE";
    source.replace(
        source.find(marker), marker.size(), lighting::kSdfTraceHlsl);
    return source;
}
} // namespace

f32 EmissiveSurfaceArea(
    const u32 shape,
    const std::array<f32, 3>& halfExtents) noexcept
{
    const f32 a = halfExtents[0];
    const f32 b = halfExtents[1];
    const f32 c = halfExtents[2];
    if (!(a > 0.0F) || !(b > 0.0F) || !(c > 0.0F))
    {
        return 0.0F;
    }

    constexpr f32 pi = std::numbers::pi_v<f32>;
    switch (shape)
    {
    case 0U: // Box
        return 8.0F * (a * b + b * c + c * a);
    case 1U: // Ellipsoid (Knud Thomsen's approximation)
    {
        constexpr f32 p = 1.6075F;
        const f32 ap = std::pow(a, p);
        const f32 bp = std::pow(b, p);
        const f32 cp = std::pow(c, p);
        return 4.0F * pi *
               std::pow((ap * bp + ap * cp + bp * cp) / 3.0F, 1.0F / p);
    }
    case 2U: // Elliptic cylinder along Y
    {
        const f32 side = 2.0F * pi * std::sqrt((a * a + c * c) * 0.5F) * 2.0F * b;
        return side + 2.0F * pi * a * c;
    }
    case 3U: // Capsule along Y
    {
        const f32 radius = std::min(a, c);
        const f32 shaft = std::max(b - radius, 0.0F);
        return 4.0F * pi * radius * radius +
               2.0F * pi * radius * (2.0F * shaft);
    }
    case 4U: // Plane (emits from both faces)
        return 8.0F * a * c;
    default:
        return 0.0F;
    }
}

f32 EquivalentSphereRadius(const f32 surfaceArea) noexcept
{
    return surfaceArea > 0.0F
        ? std::sqrt(surfaceArea / (4.0F * std::numbers::pi_v<f32>))
        : 0.0F;
}

std::array<EmissiveLight, kMaxEmissiveLights> SelectEmissiveLights(
    const std::span<const EmissiveLight> lights,
    u32& count)
{
    struct Ranked
    {
        const EmissiveLight* light;
        f32 weight;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(lights.size());
    for (const EmissiveLight& light : lights)
    {
        const f32 power = (light.radiance[0] + light.radiance[1] +
                           light.radiance[2]) *
                          light.radius * light.radius;
        if (!(power > 0.0F) || !std::isfinite(power))
        {
            continue;
        }
        const f32 distance2 = light.position[0] * light.position[0] +
                              light.position[1] * light.position[1] +
                              light.position[2] * light.position[2];
        ranked.push_back({&light, power / std::max(distance2, 1.0F)});
    }
    std::ranges::sort(
        ranked,
        [](const Ranked& a, const Ranked& b) { return a.weight > b.weight; });

    std::array<EmissiveLight, kMaxEmissiveLights> result{};
    count = static_cast<u32>(std::min<std::size_t>(ranked.size(), kMaxEmissiveLights));
    for (u32 i = 0U; i < count; ++i)
    {
        result[i] = *ranked[i].light;
    }
    return result;
}

EmissiveLightRenderer::EmissiveLightRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : device_(device)
{
    const auto cs = compiler.Compile({
        .source = LightSource(),
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false});
    lightPipeline_ = device.CreateComputePipeline({
        .computeShader = {.data = cs.bytecode.data(), .size = cs.bytecode.size()},
        .pushConstantDwords = 28U,
        .shaderResourceBuffers = 5U,
        .storageTextures = 1U,
        .sampledTextures = 4U});

    const auto vs = compiler.Compile({
        .source = kCompositeVs,
        .entryPoint = "main",
        .stage = shader::Stage::Vertex,
        .debug = false});
    const auto ps = compiler.Compile({
        .source = kCompositePs,
        .entryPoint = "main",
        .stage = shader::Stage::Pixel,
        .debug = false});
    compositePipeline_ = device.CreateGraphicsPipeline({
        .vertexShader = {.data = vs.bytecode.data(), .size = vs.bytecode.size()},
        .pixelShader = {.data = ps.bytecode.data(), .size = ps.bytecode.size()},
        .shaderResourceBuffers = 0U,
        .sampledTextures = 1U,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid,
        .cullMode = rhi::CullMode::None,
        .blendMode = rhi::BlendMode::Additive,
        .depthTest = false,
        .depthWrite = false,
        .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float},
        .colorAttachmentCount = 1U});

    dummySdf_ = device.CreateBuffer({
        .sizeBytes = 64U,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::UnorderedAccess});
    std::memset(dummySdf_->Map(), 0, static_cast<std::size_t>(dummySdf_->SizeBytes()));
    dummySdf_->Unmap();
}

void EmissiveLightRenderer::Light(
    rhi::CommandList& commands,
    const std::span<const EmissiveLight> lights,
    rhi::Texture& target,
    rhi::Texture& surfaceBaseRoughness,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& surfaceEmissionClass,
    rhi::Texture& depth,
    const u32 width,
    const u32 height,
    const lighting::LightingView& view,
    const lighting::SdfGatherInput* const sdf)
{
    ++tick_;
    std::erase_if(
        lightBuffers_,
        [this](const RetiredBuffer& buffer)
        {
            return tick_ >= buffer.retireAtTick;
        });

    if (lightPipeline_ == nullptr || width == 0U || height == 0U)
    {
        return;
    }

    u32 count = 0U;
    const auto selected = SelectEmissiveLights(lights, count);

    // Element 0: the count; then two float4 per light.
    std::vector<f32> floats((1U + 2U * kMaxEmissiveLights) * 4U, 0.0F);
    floats[0] = static_cast<f32>(count);
    for (u32 i = 0U; i < count; ++i)
    {
        f32* out = &floats[(1U + 2U * i) * 4U];
        out[0] = selected[i].position[0];
        out[1] = selected[i].position[1];
        out[2] = selected[i].position[2];
        out[3] = selected[i].radius;
        out[4] = selected[i].radiance[0];
        out[5] = selected[i].radiance[1];
        out[6] = selected[i].radiance[2];
        out[7] = selected[i].boundingRadius;
    }
    auto buffer = device_.CreateBuffer({
        .sizeBytes = floats.size() * sizeof(f32),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource});
    std::memcpy(buffer->Map(), floats.data(), floats.size() * sizeof(f32));
    buffer->Unmap();

    const bool sdfAvailable =
        sdf != nullptr && sdf->distance != nullptr && sdf->albedo != nullptr &&
        sdf->normal != nullptr && sdf->radiance != nullptr;
    const math::Double3 sdfOriginRelative = sdfAvailable
        ? sdf->originInFrameMeters - view.cameraPositionInFrameMeters
        : math::Double3{};

    const auto bits = [](const f32 value) { return std::bit_cast<u32>(value); };
    const std::array<u32, 28> constants{
        width, height, 0U, 0U,
        bits(view.forward.x), bits(view.forward.y), bits(view.forward.z),
        bits(static_cast<f32>(width) / static_cast<f32>(height)),
        bits(view.up.x), bits(view.up.y), bits(view.up.z),
        bits(std::tan(view.verticalFovRadians * 0.5F)),
        bits(std::max(view.nearPlaneMeters, 1.0e-5F)),
        bits(std::max(view.farPlaneMeters, view.nearPlaneMeters + 1.0e-4F)),
        0U, 0U,
        bits(static_cast<f32>(sdfOriginRelative.x)),
        bits(static_cast<f32>(sdfOriginRelative.y)),
        bits(static_cast<f32>(sdfOriginRelative.z)),
        bits(sdfAvailable ? sdf->voxelSize : 0.25F),
        bits(sdfAvailable ? static_cast<f32>(sdf->dimensions[0]) : 1.0F),
        bits(sdfAvailable ? static_cast<f32>(sdf->dimensions[1]) : 1.0F),
        bits(sdfAvailable ? static_cast<f32>(sdf->dimensions[2]) : 1.0F),
        bits(sdfAvailable ? 1.0F : 0.0F),
        0U, 0U, 0U, 0U};

    commands.SetComputePipeline(*lightPipeline_);
    commands.SetComputeConstants(constants);
    commands.SetComputeBuffer(0U, *buffer);
    commands.SetComputeBuffer(1U, sdfAvailable ? *sdf->distance : *dummySdf_);
    commands.SetComputeBuffer(2U, sdfAvailable ? *sdf->albedo : *dummySdf_);
    commands.SetComputeBuffer(3U, sdfAvailable ? *sdf->normal : *dummySdf_);
    commands.SetComputeBuffer(4U, sdfAvailable ? *sdf->radiance : *dummySdf_);
    commands.SetComputeStorageTexture(0U, target);
    commands.SetComputeTexture(0U, surfaceBaseRoughness);
    commands.SetComputeTexture(1U, surfaceNormalMetallic);
    commands.SetComputeTexture(2U, surfaceEmissionClass);
    commands.SetComputeTexture(3U, depth);
    commands.Dispatch((width + 7U) / 8U, (height + 7U) / 8U, 1U);

    lightBuffers_.push_back({std::move(buffer), tick_ + kBufferRetireTicks});
}

void EmissiveLightRenderer::Composite(
    rhi::CommandList& commands,
    rhi::Texture& lighting,
    rhi::Texture& sceneColor,
    const u32 width,
    const u32 height)
{
    if (compositePipeline_ == nullptr || width == 0U || height == 0U)
    {
        return;
    }
    commands.SetRenderTarget(sceneColor);
    commands.SetViewport({
        .x = 0.0F,
        .y = 0.0F,
        .width = static_cast<f32>(width),
        .height = static_cast<f32>(height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F});
    commands.SetScissor({
        .left = 0,
        .top = 0,
        .right = static_cast<i32>(width),
        .bottom = static_cast<i32>(height)});
    commands.SetGraphicsPipeline(*compositePipeline_);
    commands.SetGraphicsTexture(0U, lighting);
    commands.Draw(3U);
}
} // namespace orbit::mesh_render
