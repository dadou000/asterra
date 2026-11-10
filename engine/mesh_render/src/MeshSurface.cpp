#include <orbit/mesh_render/MeshSurface.hpp>

#include "MeshDrawRecords.hpp"

#include <orbit/mesh_render/MeshSdfScene.hpp>

#include <orbit/lighting/ProxySurface.hpp>
#include <orbit/lighting/SurfaceBuffer.hpp>
#include <orbit/mesh_import/MeshAsset.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <string>

namespace orbit::mesh_render
{
namespace
{
constexpr u64 kRecordRetireTicks = 24U;

// Shared by both stages. Per-draw data lives in a structured buffer
// (kMeshDrawRecordVectors float4s per draw) because the instance transform
// plus material do not fit the 32-dword push-constant budget.
constexpr const char* kCommon = R"(
[[vk::binding(0, 0)]]
StructuredBuffer<float4> g_records : register(t0);

struct Constants
{
    float4 right;   // xyz basis, w = x projection scale
    float4 up;      // xyz basis, w = y projection scale
    float4 forward; // xyz basis, w = reverse-Z depth scale
    float4 extra;   // x = reverse-Z depth bias, y = draw record index
};
[[vk::push_constant]] Constants g;

struct VSOutput
{
    float4 position : SV_Position;
    float3 relative : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 uv : TEXCOORD3;
};
)";

constexpr const char* kVertexShader = R"(
struct VSInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float4 tangent : TANGENT;
    [[vk::location(3)]] float2 uv : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    const uint record = (uint)g.extra.y * 6u;
    const float4 row0 = g_records[record + 0u];
    const float4 row1 = g_records[record + 1u];
    const float4 row2 = g_records[record + 2u];

    const float4 homogeneous = float4(input.position, 1.0);
    const float3 p = float3(
        dot(row0, homogeneous),
        dot(row1, homogeneous),
        dot(row2, homogeneous));

    // Uniform scale only: the linear part transforms normals as-is.
    const float3 n = float3(
        dot(row0.xyz, input.normal),
        dot(row1.xyz, input.normal),
        dot(row2.xyz, input.normal));
    const float3 t = float3(
        dot(row0.xyz, input.tangent.xyz),
        dot(row1.xyz, input.tangent.xyz),
        dot(row2.xyz, input.tangent.xyz));

    const float3 v = float3(
        dot(p, g.right.xyz),
        dot(p, g.up.xyz),
        dot(p, g.forward.xyz));

    VSOutput output;
    output.position = float4(
        v.x * g.right.w,
        v.y * g.up.w,
        v.z * g.forward.w + g.extra.x,
        v.z);
    output.relative = p;
    output.normal = n;
    output.tangent = float4(t, input.tangent.w);
    output.uv = input.uv;
    return output;
}
)";

constexpr const char* kPixelShader = R"(
[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] Texture2D g_baseColor;
[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] SamplerState g_baseColorSampler;
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] Texture2D g_normalMap;
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] SamplerState g_normalSampler;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] Texture2D g_metalRough;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] SamplerState g_metalRoughSampler;
[[vk::binding(4, 0)]] [[vk::combinedImageSampler]] Texture2D g_emissive;
[[vk::binding(4, 0)]] [[vk::combinedImageSampler]] SamplerState g_emissiveSampler;

struct PSOutput
{
    float4 sceneColor : SV_Target0;
    float4 baseRoughness : SV_Target1;
    float4 normalMetallic : SV_Target2;
    float4 emissionClass : SV_Target3;
};

PSOutput main(VSOutput input)
{
    const uint record = (uint)g.extra.y * 6u;
    const float4 baseFactor = g_records[record + 3u];
    const float4 emissiveMetal = g_records[record + 4u];
    // x roughness factor, y normal scale, z alpha cutoff, w packed metadata
    const float4 params = g_records[record + 5u];

    const float4 baseSample = g_baseColor.Sample(g_baseColorSampler, input.uv);
    const float4 base = baseSample * baseFactor;

    if (params.z > 0.0 && base.a < params.z)
    {
        discard;
    }

    const float4 mr = g_metalRough.Sample(g_metalRoughSampler, input.uv);
    const float roughness = clamp(mr.g * params.x, 0.045, 1.0);
    const float metallic = saturate(mr.b * emissiveMetal.w);
    const float3 emissive =
        g_emissive.Sample(g_emissiveSampler, input.uv).rgb * emissiveMetal.rgb;

    float3 normal = normalize(input.normal);
    const float3 viewDirection = normalize(input.relative);

    // Shade the face toward the eye (double-sided and thin geometry).
    if (dot(normal, viewDirection) > 0.0)
    {
        normal = -normal;
    }

    const float3 tangent = input.tangent.xyz;
    if (dot(tangent, tangent) > 1.0e-12)
    {
        const float3 t = normalize(tangent - normal * dot(normal, tangent));
        const float3 b = cross(normal, t) * (input.tangent.w < 0.0 ? -1.0 : 1.0);
        const float3 mapped =
            g_normalMap.Sample(g_normalSampler, input.uv).xyz * 2.0 - 1.0;
        normal = normalize(
            t * (mapped.x * params.y) +
            b * (mapped.y * params.y) +
            normal * mapped.z);
    }

    PSOutput output;
    output.sceneColor = float4(0.0, 0.0, 0.0, 1.0);
    output.baseRoughness = float4(base.rgb, roughness);
    output.normalMetallic = float4(normal, metallic);
    output.emissionClass = float4(emissive, params.w);
    return output;
}
)";

[[nodiscard]] std::string Compose(const char* body)
{
    return std::string(kCommon) + body;
}
} // namespace

std::array<f32, 12> MakeInstanceRows(
    const math::Double3x3& rotation,
    const f64 uniformScale,
    const math::Double3& origin) noexcept
{
    // Column j of the linear part is the scaled image of model axis j.
    const math::Double3 x = rotation.xAxis * uniformScale;
    const math::Double3 y = rotation.yAxis * uniformScale;
    const math::Double3 z = rotation.zAxis * uniformScale;
    const auto f = [](const f64 value) { return static_cast<f32>(value); };

    return {
        f(x.x), f(y.x), f(z.x), f(origin.x),
        f(x.y), f(y.y), f(z.y), f(origin.y),
        f(x.z), f(y.z), f(z.z), f(origin.z)};
}

MeshSurfaceRenderer::MeshSurfaceRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : device_(device)
{
    const auto vs = compiler.Compile({
        .source = Compose(kVertexShader),
        .entryPoint = "main",
        .stage = shader::Stage::Vertex,
        .debug = false});
    const auto ps = compiler.Compile({
        .source = Compose(kPixelShader),
        .entryPoint = "main",
        .stage = shader::Stage::Pixel,
        .debug = false});

    static constexpr std::array<rhi::VertexAttribute, 4> attributes{{
        {0U, rhi::VertexFormat::Float3,
         offsetof(mesh_import::MeshVertex, position)},
        {1U, rhi::VertexFormat::Float3,
         offsetof(mesh_import::MeshVertex, normal)},
        {2U, rhi::VertexFormat::Float4,
         offsetof(mesh_import::MeshVertex, tangent)},
        {3U, rhi::VertexFormat::Float2,
         offsetof(mesh_import::MeshVertex, uv)}}};

    pipeline_ = device.CreateGraphicsPipeline({
        .vertexShader = {.data = vs.bytecode.data(), .size = vs.bytecode.size()},
        .pixelShader = {.data = ps.bytecode.data(), .size = ps.bytecode.size()},
        .vertexAttributes = attributes,
        .vertexStrideBytes = sizeof(mesh_import::MeshVertex),
        .pushConstantDwords = kMeshSurfacePushDwords,
        .shaderResourceBuffers = 1U,
        .sampledTextures = kMeshTextureSlots,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid,
        // Thin and double-sided geometry is shaded toward the eye in the
        // pixel shader, so no face is culled.
        .cullMode = rhi::CullMode::None,
        .blendMode = rhi::BlendMode::Opaque,
        .depthCompare = rhi::DepthCompare::GreaterEqual,
        .depthTest = true,
        .depthWrite = true,
        .colorAttachmentFormats = {
            rhi::TextureFormat::RGBA16_Float,
            rhi::TextureFormat::RGBA16_Float,
            rhi::TextureFormat::RGBA16_Float,
            rhi::TextureFormat::RGBA16_Float},
        .colorAttachmentCount = 4U});
}

void MeshSurfaceRenderer::Draw(
    rhi::CommandList& commands,
    MeshLibrary& library,
    const std::span<const MeshInstance> instances,
    rhi::Texture& sceneColor,
    rhi::Texture& surfaceBaseRoughness,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& surfaceEmissionClass,
    rhi::Texture& depth,
    const u32 width,
    const u32 height,
    const lighting::LightingView& view,
    const SdfExtraGeometry* sdfExtra)
{
    ++tick_;

    // Uploads and retirement happen outside any render pass.
    library.Pump(commands);

    if (sdfScene_ != nullptr)
    {
        sdfScene_->Update(
            commands, instances, view.cameraPositionInFrameMeters, sdfExtra);
    }

    std::erase_if(
        records_,
        [this](const RetiredRecords& records)
        {
            return tick_ >= records.retireAtTick;
        });

    if (pipeline_ == nullptr || width == 0U || height == 0U)
    {
        return;
    }

    std::vector<detail::DrawCall> draws;
    std::vector<f32> records;

    const f32 metadata = lighting::EncodeSurfaceMetadata(
        lighting::SurfaceClass::RigidGeometry,
        lighting::SurfaceRepresentation::StaticMesh);

    detail::BuildDrawRecords(instances, metadata, draws, records);

    if (draws.empty())
    {
        return;
    }

    auto buffer = device_.CreateBuffer({
        .sizeBytes = records.size() * sizeof(f32),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource});
    std::memcpy(buffer->Map(), records.data(), records.size() * sizeof(f32));
    buffer->Unmap();

    const auto proxyConstants = lighting::PackProxySurfaceConstants(
        view, width, height, math::Double3{}, {});

    const std::array<rhi::Texture*, 4> targets{
        &sceneColor,
        &surfaceBaseRoughness,
        &surfaceNormalMetallic,
        &surfaceEmissionClass};

    commands.SetRenderTargets(targets, &depth);
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
    commands.SetGraphicsPipeline(*pipeline_);

    for (const detail::DrawCall& draw : draws)
    {
        // The first twelve dwords are exactly the proxy pass's camera basis
        // and projection scales, so meshes land where proxies do; dword 15
        // of the proxy block is the reverse-Z depth bias.
        std::array<u32, kMeshSurfacePushDwords> constants{};
        std::copy_n(proxyConstants.begin(), 12U, constants.begin());
        constants[12] = proxyConstants[15];
        constants[13] = std::bit_cast<u32>(static_cast<f32>(draw.record));

        const auto& model = *draw.model;
        const auto& material = model.Materials()[draw.part->material];

        commands.SetGraphicsConstants(constants);
        commands.SetGraphicsBuffer(0U, *buffer);
        for (u32 slot = 0U; slot < kMeshTextureSlots; ++slot)
        {
            rhi::Texture* texture = model.Texture(material.texture[slot]);
            commands.SetGraphicsTexture(
                slot,
                texture != nullptr
                    ? *texture
                    : library.DefaultTexture(
                          static_cast<MeshTextureRole>(slot)));
        }
        commands.SetVertexBuffer(
            *model.VertexBuffer(), sizeof(mesh_import::MeshVertex));
        commands.SetIndexBuffer(*model.IndexBuffer(), rhi::IndexFormat::UInt32);
        commands.DrawIndexed(draw.part->indexCount, draw.part->firstIndex, 0);
    }

    records_.push_back({std::move(buffer), tick_ + kRecordRetireTicks});
}
} // namespace orbit::mesh_render
