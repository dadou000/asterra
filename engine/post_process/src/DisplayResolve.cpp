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
    float headroomRatio;
    float shoulderStart;
    float shoulderStrength;
    float3 padding;
};

[[vk::push_constant]]
Constants g;

float Luminance(float3 color)
{
    return dot(max(color, 0.0), float3(0.2126, 0.7152, 0.0722));
}

float3 ToneMapProduction(float3 sceneLinear)
{
    sceneLinear = max(sceneLinear, 0.0);

    if (g.toneMapEnabled <= 0.5)
    {
        return sceneLinear;
    }

    const float luminance =
        max(Luminance(sceneLinear), 0.0);

    if (luminance <= 1.0e-6)
    {
        return 0.0;
    }

    const float headroom =
        max(g.headroomRatio, 1.0);
    const float shoulderStart =
        clamp(
            g.shoulderStart,
            0.0,
            max(headroom - 1.0e-4, 0.0));

    float mapped = luminance;

    if (luminance > shoulderStart &&
        headroom > shoulderStart + 1.0e-4)
    {
        const float remaining =
            headroom - shoulderStart;
        const float scale =
            max(
                remaining *
                    max(g.shoulderStrength, 1.0e-3),
                1.0e-4);

        mapped =
            shoulderStart +
            remaining *
                (1.0 -
                 exp(
                     -(luminance - shoulderStart) /
                     scale));
        mapped =
            clamp(mapped, 0.0, headroom);
    }
    else
    {
        mapped =
            min(mapped, headroom);
    }

    return sceneLinear * (mapped / luminance);
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
            ToneMapProduction(displayLinear);
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
            .pushConstantDwords = 8U,
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
            .depthWrite = false,
            .colorAttachmentFormats = {
                rhi::TextureFormat::RGBA16_Float
            },
            .colorAttachmentCount = 1U
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

    const f32 headroom =
        DisplayHeadroomRatio(
            settings.toneMapping);

    const std::array<u32, 8> constants{
        bits(std::max(settings.exposureScale, 0.0F)),
        bits(settings.toneMapping.enabled ? 1.0F : 0.0F),
        bits(headroom),
        bits(std::clamp(
            settings.toneMapping.shoulderStart,
            0.0F,
            headroom)),
        bits(std::max(
            settings.toneMapping.shoulderStrength,
            1.0e-3F)),
        0U,
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
