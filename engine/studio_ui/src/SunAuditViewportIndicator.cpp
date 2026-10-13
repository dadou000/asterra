#include <orbit/studio_ui/SunAuditViewportIndicator.hpp>

#include <array>
#include <bit>

namespace orbit::studio_ui
{
namespace
{
constexpr const char* kVs = R"(
struct VSOutput { float4 position : SV_Position; };
VSOutput main(uint id : SV_VertexID)
{
    float2 p[6] = {
        float2(-1,-1),float2(-1,1),float2(1,-1),
        float2(1,-1),float2(-1,1),float2(1,1)
    };
    VSOutput output;
    output.position = float4(p[id],0,1);
    return output;
}
)";
constexpr const char* kPs = R"(
struct Constants { float4 display; };
[[vk::push_constant]] Constants g;
float4 main(float4 position : SV_Position) : SV_Target0
{
    // 180 x 18 pixel swatch, after exposure, bloom, tone mapping and
    // output transform. No auto-exposure dependency and no scene modification.
    if (position.x >= min(g.display.z, 180.0) ||
        position.y >= min(g.display.w, 18.0)) discard;
    float3 c = g.display.x < 1.5 ? float3(1.0,0.16,0.07) :
        (g.display.x < 2.5 ? float3(0.06,0.9,0.22) :
         float3(0.12,0.38,1.0));
    if (position.y >= 12.0) c = g.display.y > 0.5
        ? float3(1.0,1.0,1.0) : c * 0.18;
    return float4(c, 1.0);
}
)";
} // namespace

SunAuditViewportIndicator::SunAuditViewportIndicator(
    rhi::Device& device, const shader::Compiler& compiler)
{
    const auto vs = compiler.Compile({
        .source = kVs, .entryPoint = "main",
        .stage = shader::Stage::Vertex, .debug = false});
    const auto ps = compiler.Compile({
        .source = kPs, .entryPoint = "main",
        .stage = shader::Stage::Pixel, .debug = false});
    pipeline_ = device.CreateGraphicsPipeline({
        .vertexShader = {.data = vs.bytecode.data(), .size = vs.bytecode.size()},
        .pixelShader = {.data = ps.bytecode.data(), .size = ps.bytecode.size()},
        .vertexAttributes = {}, .vertexStrideBytes = 0U,
        .pushConstantDwords = 4U,
        .shaderResourceBuffers = 0U, .sampledTextures = 0U,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid, .cullMode = rhi::CullMode::None,
        .blendMode = rhi::BlendMode::Opaque, .depthTest = false,
        .depthWrite = false,
        .colorAttachmentFormats = {rhi::TextureFormat::RGBA8_UNorm},
        .colorAttachmentCount = 1U});
}

void SunAuditViewportIndicator::Draw(
    rhi::CommandList& commands, rhi::Texture& display,
    const u32 width, const u32 height, const u8 method, const bool debug) const
{
    if (width == 0U || height == 0U || method == 0U) return;
    const auto bits = [](const f32 v) { return std::bit_cast<u32>(v); };
    const std::array<u32, 4> constants{
        bits(static_cast<f32>(method)),
        bits(debug ? 1.0F : 0.0F),
        bits(static_cast<f32>(width)),
        bits(static_cast<f32>(height))
    };
    commands.SetRenderTarget(display);
    commands.SetViewport({.x = 0.0F, .y = 0.0F,
        .width = static_cast<f32>(width), .height = static_cast<f32>(height),
        .minDepth = 0.0F, .maxDepth = 1.0F});
    commands.SetScissor({.left = 0, .top = 0,
        .right = static_cast<i32>(width), .bottom = static_cast<i32>(height)});
    commands.SetGraphicsPipeline(*pipeline_);
    commands.SetGraphicsConstants(constants);
    commands.Draw(6U);
}
} // namespace orbit::studio_ui
