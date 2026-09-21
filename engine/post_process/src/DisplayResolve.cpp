#include <orbit/post_process/DisplayResolve.hpp>

#include <array>
#include <bit>
#include <algorithm>

namespace orbit::post_process
{
namespace
{
constexpr const char* kFullscreenVertexShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    const float2 positions[6] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0, -1.0),
        float2( 1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0,  1.0)
    };

    const float2 uvs[6] =
    {
        float2(0.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 1.0),
        float2(1.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 0.0)
    };

    VSOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}
)";

constexpr const char* kDisplayResolvePixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_source;

[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sourceSampler;

struct Constants
{
    float exposureScale;
    float toneMapEnabled;
    float2 padding;
};

[[vk::push_constant]]
Constants g;

float3 ToneMapReinhard(float3 sceneLinear)
{
    sceneLinear = max(sceneLinear, 0.0);
    return sceneLinear / (1.0 + sceneLinear);
}

float4 main(VSOutput input) : SV_Target0
{
    const float4 source =
        g_source.Sample(g_sourceSampler, input.uv);

    float3 displayLinear =
        max(source.rgb, 0.0) *
        max(g.exposureScale, 0.0);

    if (g.toneMapEnabled > 0.5)
    {
        displayLinear =
            ToneMapReinhard(displayLinear);
    }

    return float4(displayLinear, source.a);
}
)";
} // namespace

DisplayResolveRenderer::DisplayResolveRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto vertex =
        compiler.Compile({
            .source = kFullscreenVertexShader,
            .entryPoint = "main",
            .stage = shader::Stage::Vertex,
            .debug = false
        });

    const auto pixel =
        compiler.Compile({
            .source = kDisplayResolvePixelShader,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

    pipeline_ =
        device.CreateGraphicsPipeline({
            .vertexShader = {
                .data = vertex.bytecode.data(),
                .size = vertex.bytecode.size()
            },
            .pixelShader = {
                .data = pixel.bytecode.data(),
                .size = pixel.bytecode.size()
            },
            .vertexAttributes = {},
            .vertexStrideBytes = 0U,
            .pushConstantDwords = 4U,
            .shaderResourceBuffers = 0U,
            .sampledTextures = 1U,
            .topology =
                rhi::PrimitiveTopology::TriangleList,
            .fillMode =
                rhi::FillMode::Solid,
            .cullMode =
                rhi::CullMode::None,
            .blendMode =
                rhi::BlendMode::Opaque,
            .depthTest = false,
            .depthWrite = false
        });
}

void DisplayResolveRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& sourceHdr,
    rhi::Texture& targetDisplayLinear,
    const u32 width,
    const u32 height,
    const DisplayResolveSettings& settings)
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

    const std::array<u32, 4> constants{
        bits(std::max(settings.exposureScale, 0.0F)),
        bits(settings.toneMapEnabled ? 1.0F : 0.0F),
        0U,
        0U
    };

    commands.SetRenderTarget(targetDisplayLinear);
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
    commands.SetGraphicsTexture(0U, sourceHdr);
    commands.Draw(6U);
}
} // namespace orbit::post_process
