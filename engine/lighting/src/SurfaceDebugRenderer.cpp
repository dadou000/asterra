#include <orbit/lighting/SurfaceDebugRenderer.hpp>

#include <array>
#include <bit>

namespace orbit::lighting
{
namespace
{
constexpr const char* kVs = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    const float2 p[6] =
    {
        float2(-1,-1),
        float2(-1, 1),
        float2( 1,-1),
        float2( 1,-1),
        float2(-1, 1),
        float2( 1, 1)
    };

    const float2 uv[6] =
    {
        float2(0,1),
        float2(0,0),
        float2(1,1),
        float2(1,1),
        float2(0,0),
        float2(1,0)
    };

    VSOutput o;
    o.position = float4(p[vertexId], 0, 1);
    o.uv = uv[vertexId];
    return o;
}
)";

constexpr const char* kPs = R"(
[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_source;

[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sampler;

struct Constants
{
    float mode;
    float3 padding;
};
[[vk::push_constant]] Constants g;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float3 MetadataColor(float packed)
{
    const float cls = floor(max(packed, 0.0));
    const float rep = round(frac(max(packed, 0.0)) * 16.0);

    const float3 classColor =
        frac(float3(
            cls * 0.173 + 0.11,
            cls * 0.371 + 0.27,
            cls * 0.619 + 0.43));

    const float representationBand =
        saturate(rep / 7.0);

    return lerp(
        classColor,
        float3(
            representationBand,
            1.0 - representationBand,
            0.5),
        0.35);
}

float4 main(VSOutput input) : SV_Target0
{
    const float4 value =
        g_source.Sample(g_sampler, input.uv);

    if (g.mode < 1.5)
    {
        // Base color in the main image; roughness as a right-side strip.
        if (input.uv.x > 0.88)
        {
            return float4(value.aaa, 1.0);
        }
        return float4(max(value.rgb, 0.0), 1.0);
    }

    if (g.mode < 2.5)
    {
        // Normal remapped from [-1,1], metallic in the side strip.
        if (input.uv.x > 0.88)
        {
            return float4(value.aaa, 1.0);
        }
        return float4(
            normalize(value.xyz) * 0.5 + 0.5,
            1.0);
    }

    // Emission log-compressed; packed class/representation in side strip.
    if (input.uv.x > 0.88)
    {
        return float4(
            MetadataColor(value.a),
            1.0);
    }

    const float3 emission =
        log2(1.0 + max(value.rgb, 0.0)) / 8.0;
    return float4(
        saturate(emission),
        1.0);
}
)";
} // namespace

SurfaceDebugRenderer::SurfaceDebugRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto vs =
        compiler.Compile({
            .source = kVs,
            .entryPoint = "main",
            .stage = shader::Stage::Vertex,
            .debug = false
        });

    const auto ps =
        compiler.Compile({
            .source = kPs,
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
            .pushConstantDwords = 4U,
            .sampledTextures = 1U,
            .topology =
                rhi::PrimitiveTopology::TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Opaque,
            .depthTest = false,
            .depthWrite = false,
            .colorAttachmentFormats = {
                rhi::TextureFormat::RGBA16_Float
            },
            .colorAttachmentCount = 1U
        });
}

void SurfaceDebugRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& source,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    const SurfaceDebugMode mode)
{
    const std::array<u32, 4> constants{
        std::bit_cast<u32>(
            static_cast<f32>(mode)),
        0U, 0U, 0U
    };

    commands.SetRenderTarget(target);
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
    commands.SetGraphicsTexture(0U, source);
    commands.Draw(6U);
}
} // namespace orbit::lighting
