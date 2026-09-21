#include <orbit/terrain_render/UniformPlanetRenderer.hpp>
#include <orbit/math/Matrix.hpp>
#include "TerrainSurfaceShader.hpp"
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <future>
#include <optional>
#include <stdexcept>

namespace orbit::terrain_render
{
namespace
{
constexpr const char* kVertexShader = R"(
struct Constants
{
    row_major float4x4 g_mvp;
    float4 g_east;
    float4 g_up;
    float4 g_north;
    float4 g_planet; // radius, observer altitude, resolution, face
};
[[vk::push_constant]] Constants g_pc;
[[vk::binding(0, 0)]]
ByteAddressBuffer g_samples : register(t0);
struct VSOutput
{
    float4 position : SV_Position;
    float elevation : TEXCOORD0;
    float4 biome0 : TEXCOORD1;
    float4 biome1 : TEXCOORD2;
    float3 terrainNormal : TEXCOORD3;
    float3 surfaceDirection : TEXCOORD4;
    float waterDepth : TEXCOORD5;
    float3 localPosition : TEXCOORD6;
    float spacingMeters : TEXCOORD7;
    float3 worldPosition : TEXCOORD8;
    float3 bodyFixedNormal : TEXCOORD9;
    float3 bodyFixedSurfaceDirection : TEXCOORD10;
    float horizonClip : SV_ClipDistance0;
};
float3 Direction(uint2 cell)
{
    float2 uv = 2.0 * float2(cell) / (g_pc.g_planet.z - 1.0) - 1.0;
    uint face = (uint)g_pc.g_planet.w;
    float3 p;
    if (face == 0u) p = float3(1, uv.y, -uv.x);
    else if (face == 1u) p = float3(-1, uv.y, uv.x);
    else if (face == 2u) p = float3(uv.x, 1, -uv.y);
    else if (face == 3u) p = float3(uv.x, -1, uv.y);
    else if (face == 4u) p = float3(uv.x, uv.y, 1);
    else p = float3(-uv.x, uv.y, -1);
    return normalize(p);
}
uint Address(uint2 cell)
{
    uint n = (uint)g_pc.g_planet.z;
    return (((uint)g_pc.g_planet.w * n + cell.y) * n + cell.x) * 20u;
}
// Decodes the octahedral-packed normal baked once at mesh build time
// (see PackOctahedralNormal in UniformPlanetMesh.cpp) instead of
// finite-differencing neighbor heights here every frame.
float3 UnpackOctahedralNormal(uint packed)
{
    int2 signedLanes = int2((int)(packed << 16) >> 16, (int)packed >> 16);
    float2 e = float2(signedLanes) / 32767.0;
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
float3 LocalDirection(float3 v)
{
    return float3(dot(v, g_pc.g_east.xyz), dot(v, g_pc.g_up.xyz), dot(v, g_pc.g_north.xyz));
}
float4 Unpack(uint p)
{
    return float4(p & 255u, (p >> 8u) & 255u, (p >> 16u) & 255u, (p >> 24u) & 255u) / 255.0;
}
VSOutput main(uint id : SV_VertexID)
{
    uint2 cell = uint2(id % (uint)g_pc.g_planet.z, id / (uint)g_pc.g_planet.z);
    uint address = Address(cell);
    uint4 data = g_samples.Load4(address);
    uint packedNormal = g_samples.Load(address + 16u);
    float height = asfloat(data.x) + asfloat(data.y);
    float3 direction = Direction(cell);
    float3 normal = UnpackOctahedralNormal(packedNormal);
    float3 localDir = LocalDirection(direction);
    float3 local = localDir * (g_pc.g_planet.x + height);
    local.y = (localDir.y - 1.0) * g_pc.g_planet.x + localDir.y * height - g_pc.g_planet.y;
    VSOutput output;
    output.position = mul(float4(local, 1), g_pc.g_mvp);
    output.elevation = height;
    output.biome0 = Unpack(data.z);
    output.biome1 = Unpack(data.w);
    output.terrainNormal = LocalDirection(normal);
    output.surfaceDirection = localDir;
    output.bodyFixedNormal = normal;
    output.bodyFixedSurfaceDirection = direction;
    output.waterDepth = asfloat(data.y);
    output.localPosition = local;
    // Unlike localPosition/surfaceDirection above (deliberately
    // observer-relative, see LocalDirection), `direction` here is
    // computed purely from the cube-sphere cell/face -- already
    // camera-independent, so no extra reconstruction is needed the
    // way TerrainPreviewRenderer's vertex shader requires (it has no
    // equivalent observer-independent direction available).
    output.worldPosition = direction * g_pc.g_planet.x;
    // This mesh has no clipmap levels to derive a real sample spacing
    // from -- approximate it from a cube face's ~90-degree arc over
    // its resolution, just to fade the shader-only detail-normal
    // bump (see ApplyDetailNormal) in the same way the clipmap
    // renderer does.
    output.spacingMeters = (g_pc.g_planet.x * 1.5707963) / max(g_pc.g_planet.z - 1.0, 1.0);
    // Full closed planet; depth testing handles occlusion, no clipmap horizon cut.
    output.horizonClip = 1.0;
    return output;
}
)";
std::unique_ptr<rhi::Buffer> Upload(rhi::Device& device, const void* data, const u64 size,
    const rhi::BufferUsage usage, const rhi::ResourceState state)
{
    auto buffer = device.CreateBuffer({.sizeBytes = size, .usage = usage,
        .memory = rhi::MemoryUsage::HostVisible, .initialState = state});
    std::memcpy(buffer->Map(), data, static_cast<std::size_t>(size));
    buffer->Unmap();
    return buffer;
}
}
class UniformPlanetRenderer::Impl
{
public:
    rhi::Device& device;
    world::PlanetDefinition planet;
    std::shared_ptr<const terrain::TerrainSource> source;
    TerrainPreviewConfig config;
    i32 requested{-1};
    i32 active{-1};
    std::future<terrain_stream::UniformPlanetMesh> future;
    std::optional<terrain_stream::UniformPlanetMesh> ready;
    std::unique_ptr<rhi::Buffer> samples;
    std::unique_ptr<rhi::Buffer> indices;
    std::unique_ptr<rhi::GraphicsPipeline> pipeline;
    u32 resolution{};
    u32 indexCount{};
    Impl(rhi::Device& d, const shader::Compiler& compiler, const world::PlanetDefinition p,
        std::shared_ptr<const terrain::TerrainSource> s, const TerrainPreviewConfig c)
        : device(d), planet(p), source(std::move(s)), config(c)
    {
        const auto vs = compiler.Compile({.source = kVertexShader, .entryPoint = "main", .stage = shader::Stage::Vertex});
        const auto ps = compiler.Compile({.source = detail::kTerrainSurfacePixelShader, .entryPoint = "main", .stage = shader::Stage::Pixel});
        pipeline = device.CreateGraphicsPipeline({
            .vertexShader = {vs.bytecode.data(), vs.bytecode.size()},
            .pixelShader = {ps.bytecode.data(), ps.bytecode.size()},
            .pushConstantDwords = 32, .shaderResourceBuffers = 1,
            .cullMode = rhi::CullMode::Back, .depthCompare = rhi::DepthCompare::GreaterEqual,
            .depthTest = true,
            .depthWrite = true,
            .colorAttachmentFormats = {
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float
            },
            .colorAttachmentCount = 4U});
    }
    void Poll()
    {
        if (future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            auto mesh = future.get();
            if (static_cast<i32>(mesh.lod) == requested) ready = std::move(mesh);
        }
        if (!future.valid() && !ready && requested >= 0 && requested != active)
        {
            const auto lod = static_cast<u32>(requested);
            future = std::async(std::launch::async, [p = planet, s = source, lod]
            {
                return terrain_stream::BuildUniformPlanetMesh(p, *s, lod);
            });
        }
    }
};
UniformPlanetRenderer::UniformPlanetRenderer(rhi::Device& device, const shader::Compiler& compiler,
    const world::PlanetDefinition planet, std::shared_ptr<const terrain::TerrainSource> source,
    const TerrainPreviewConfig config)
    : impl_(std::make_unique<Impl>(device, compiler, planet, std::move(source), config)) {}
UniformPlanetRenderer::~UniformPlanetRenderer() = default;
void UniformPlanetRenderer::RequestLod(const i32 lod)
{
    if (lod < -1 || lod > static_cast<i32>(terrain_stream::kMaximumUniformPlanetLod))
        throw std::invalid_argument("Invalid fixed planet LOD.");
    if (impl_->requested == lod) return;
    impl_->requested = lod;
    impl_->ready.reset();
}
void UniformPlanetRenderer::Poll() { impl_->Poll(); }
bool UniformPlanetRenderer::HasReadyMesh() const { return impl_->ready.has_value(); }
void UniformPlanetRenderer::CommitReadyMesh()
{
    if (!impl_->ready) return;
    const auto& mesh = *impl_->ready;
    // Free the outgoing LOD's buffers before allocating the incoming
    // one's, rather than briefly holding both -- at LOD7+ a single
    // copy is already gigabytes, so the caller (who has already
    // waited out in-flight GPU work) shouldn't have to fit two.
    impl_->samples.reset();
    impl_->indices.reset();
    impl_->samples = Upload(impl_->device, mesh.samples.data(), mesh.samples.size() * sizeof(mesh.samples[0]),
        rhi::BufferUsage::Structured, rhi::ResourceState::ShaderResource);
    impl_->indices = Upload(impl_->device, mesh.indices.data(), mesh.indices.size() * sizeof(u32),
        rhi::BufferUsage::Index, rhi::ResourceState::IndexBuffer);
    impl_->resolution = mesh.resolution;
    impl_->indexCount = static_cast<u32>(mesh.indices.size());
    impl_->active = static_cast<i32>(mesh.lod);
    impl_->ready.reset();
}
i32 UniformPlanetRenderer::RequestedLod() const { return impl_->requested; }
i32 UniformPlanetRenderer::ActiveLod() const { return impl_->active; }
bool UniformPlanetRenderer::Building() const { return impl_->requested >= 0 && impl_->requested != impl_->active; }
void UniformPlanetRenderer::Draw(rhi::CommandList& commands, const world::WorldPosition& observer,
    const world::SurfaceFrame& frame, const u32 width, const u32 height, const TerrainPreviewCamera& camera)
{
    if (impl_->active < 0 || width == 0 || height == 0) return;
    const auto view = math::LookAtLH({0, 0, 0}, camera.forward, camera.up);
    const auto projection = math::PerspectiveReverseZLH(impl_->config.verticalFovRadians,
        static_cast<f32>(width) / static_cast<f32>(height), impl_->config.nearPlaneMeters, impl_->config.farPlaneMeters);
    const auto matrix = math::Multiply(view, projection);
    std::array<u32, 32> constants{};
    std::memcpy(constants.data(), matrix.values.data(), sizeof(matrix.values));
    const auto store = [&](const u32 i, const f64 v) { constants[i] = std::bit_cast<u32>(static_cast<f32>(v)); };
    const auto axis = [&](const u32 i, const math::Double3& a) { store(i, a.x); store(i + 1, a.y); store(i + 2, a.z); };
    axis(16, frame.east); axis(20, frame.up); axis(24, frame.north);
    store(28, impl_->planet.radiusMeters);
    store(29, math::Length(observer.meters) - impl_->planet.radiusMeters);
    store(30, impl_->resolution);
    commands.SetViewport({0, 0, static_cast<f32>(width), static_cast<f32>(height), 0, 1});
    commands.SetScissor({0, 0, static_cast<i32>(width), static_cast<i32>(height)});
    commands.SetGraphicsPipeline(*impl_->pipeline);
    commands.SetGraphicsBuffer(0, *impl_->samples);
    commands.SetIndexBuffer(*impl_->indices, rhi::IndexFormat::UInt32);
    for (u32 face = 0; face < 6; ++face)
    {
        store(31, face);
        commands.SetGraphicsConstants(constants);
        commands.DrawIndexed(impl_->indexCount);
    }
}
}
