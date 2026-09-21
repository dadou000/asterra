#include <orbit/post_process/HighlightEffects.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace orbit::post_process
{
namespace
{
[[nodiscard]] f32 SoftKnee(
    const f32 value,
    const f32 threshold,
    const f32 knee) noexcept
{
    const f32 safeThreshold =
        std::max(threshold, 0.0F);
    const f32 safeKnee =
        std::max(knee, 1.0e-5F);

    const f32 delta =
        value - safeThreshold;
    const f32 soft =
        std::clamp(
            delta + safeKnee,
            0.0F,
            2.0F * safeKnee);

    const f32 curved =
        soft * soft /
        (4.0F * safeKnee);

    return
        std::max(delta, 0.0F) +
        curved;
}

constexpr const char* kFullscreenVs = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    const float2 p[6] =
    {
        float2(-1,-1), float2(-1,1), float2(1,-1),
        float2(1,-1), float2(-1,1), float2(1,1)
    };
    const float2 uv[6] =
    {
        float2(0,1), float2(0,0), float2(1,1),
        float2(1,1), float2(0,0), float2(1,0)
    };

    VSOutput output;
    output.position = float4(p[vertexId], 0, 1);
    output.uv = uv[vertexId];
    return output;
}
)";

constexpr const char* kHighlightPs = R"(
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

    float invWidth;
    float invHeight;
    float shoulderStrength;
    float tonePadding;

    float bloomEnabled;
    float bloomThreshold;
    float bloomKnee;
    float bloomStrength;

    float bloomRadiusPixels;
    float glareEnabled;
    float glareThreshold;
    float glareStrength;

    float glareRadiusPixels;
    float flareEnabled;
    float flareThreshold;
    float flareStrength;

    float flareCompactness;
    float flareGhostScale;
    float debugMode;
    float padding;
};
[[vk::push_constant]] Constants g;

float Luminance(float3 color)
{
    return dot(max(color, 0.0), float3(0.2126, 0.7152, 0.0722));
}

float SoftKnee(float value, float threshold, float knee)
{
    threshold = max(threshold, 0.0);
    knee = max(knee, 1.0e-5);
    const float delta = value - threshold;
    const float soft = clamp(delta + knee, 0.0, 2.0 * knee);
    return max(delta, 0.0) + soft * soft / (4.0 * knee);
}

float3 Extract(float3 exposed, float amount)
{
    const float lum = max(Luminance(exposed), 1.0e-6);
    return exposed * (amount / lum);
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

float3 SampleExposed(float2 uv)
{
    return max(g_source.SampleLevel(g_sourceSampler, saturate(uv), 0).rgb, 0.0)
        * max(g.exposureScale, 0.0);
}

float4 main(VSOutput input) : SV_Target0
{
    const float2 texel = float2(g.invWidth, g.invHeight);
    const float3 center = SampleExposed(input.uv);
    const float centerLum = Luminance(center);

    const float radius = max(g.bloomRadiusPixels, 0.5);
    const float2 dx = float2(texel.x * radius, 0.0);
    const float2 dy = float2(0.0, texel.y * radius);
    const float2 dd = float2(texel.x * radius * 0.7071, texel.y * radius * 0.7071);

    float3 bloom = 0.0;
    if (g.bloomEnabled > 0.5)
    {
        const float3 taps[9] =
        {
            center,
            SampleExposed(input.uv + dx),
            SampleExposed(input.uv - dx),
            SampleExposed(input.uv + dy),
            SampleExposed(input.uv - dy),
            SampleExposed(input.uv + dd),
            SampleExposed(input.uv - dd),
            SampleExposed(input.uv + float2(dd.x, -dd.y)),
            SampleExposed(input.uv + float2(-dd.x, dd.y))
        };

        [unroll]
        for (uint i = 0; i < 9; ++i)
        {
            const float amount =
                SoftKnee(
                    Luminance(taps[i]),
                    g.bloomThreshold,
                    g.bloomKnee);
            bloom += Extract(taps[i], amount);
        }

        bloom *= g.bloomStrength / 9.0;
    }

    float3 glare = 0.0;
    if (g.glareEnabled > 0.5)
    {
        const float glareRadius =
            max(g.glareRadiusPixels, 1.0);

        [unroll]
        for (int i = 1; i <= 4; ++i)
        {
            const float distanceScale =
                glareRadius * float(i) / 4.0;

            const float2 gx =
                float2(texel.x * distanceScale, 0.0);
            const float2 gy =
                float2(0.0, texel.y * distanceScale);

            const float3 samples[4] =
            {
                SampleExposed(input.uv + gx),
                SampleExposed(input.uv - gx),
                SampleExposed(input.uv + gy),
                SampleExposed(input.uv - gy)
            };

            [unroll]
            for (uint j = 0; j < 4; ++j)
            {
                const float excess =
                    max(
                        Luminance(samples[j]) -
                            g.glareThreshold,
                        0.0);
                glare +=
                    Extract(samples[j], excess) /
                    float(i);
            }
        }

        glare *= g.glareStrength / 8.333333;
    }

    float neighborhoodLum = 0.0;
    {
        const float compactRadius =
            max(g.bloomRadiusPixels * 0.6, 1.0);
        const float2 cx =
            float2(texel.x * compactRadius, 0.0);
        const float2 cy =
            float2(0.0, texel.y * compactRadius);

        neighborhoodLum =
            (Luminance(SampleExposed(input.uv + cx)) +
             Luminance(SampleExposed(input.uv - cx)) +
             Luminance(SampleExposed(input.uv + cy)) +
             Luminance(SampleExposed(input.uv - cy))) * 0.25;
    }

    const float compactness =
        centerLum /
        max(neighborhoodLum, 1.0e-5);

    float3 flare = 0.0;
    if (g.flareEnabled > 0.5 &&
        centerLum > g.flareThreshold &&
        compactness > g.flareCompactness)
    {
        const float sourceExcess =
            centerLum - g.flareThreshold;
        const float2 centered =
            input.uv - 0.5;
        const float2 ghostUv =
            0.5 -
            centered * max(g.flareGhostScale, 0.0);

        const float3 ghost =
            SampleExposed(ghostUv);

        flare =
            Extract(center, sourceExcess) *
                g.flareStrength +
            Extract(
                ghost,
                max(
                    Luminance(ghost) -
                        g.flareThreshold,
                    0.0)) *
                (g.flareStrength * 0.35);
    }

    if (g.debugMode > 0.5 && g.debugMode < 1.5)
    {
        return float4(bloom, 1.0);
    }
    if (g.debugMode >= 1.5 && g.debugMode < 2.5)
    {
        return float4(glare, 1.0);
    }
    if (g.debugMode >= 2.5)
    {
        return float4(flare, 1.0);
    }

    float3 result =
        center +
        bloom +
        glare +
        flare;

    if (g.toneMapEnabled > 0.5)
    {
        result =
            ToneMapProduction(result);
    }

    return float4(result, 1.0);
}
)";
} // namespace

HighlightClassification
ClassifyHighlight(
    const f32 exposedLuminance,
    const f32 neighborhoodLuminance,
    const HighlightEffectsConfig& config) noexcept
{
    const f32 luminance =
        std::max(exposedLuminance, 0.0F);
    const f32 neighborhood =
        std::max(neighborhoodLuminance, 1.0e-6F);

    HighlightClassification result{};

    if (config.bloomEnabled)
    {
        result.bloom =
            SoftKnee(
                luminance,
                config.bloomThreshold,
                config.bloomKnee) *
            std::max(config.bloomStrength, 0.0F);
    }

    if (config.glareEnabled)
    {
        result.glare =
            std::max(
                luminance -
                    std::max(config.glareThreshold, 0.0F),
                0.0F) *
            std::max(config.glareStrength, 0.0F);
    }

    if (config.flareEnabled)
    {
        const f32 compactness =
            luminance / neighborhood;

        if (luminance >
                std::max(config.flareThreshold, 0.0F) &&
            compactness >
                std::max(config.flareCompactness, 1.0F))
        {
            result.flare =
                (luminance -
                 std::max(config.flareThreshold, 0.0F)) *
                std::max(config.flareStrength, 0.0F);
        }
    }

    return result;
}

HighlightEffectsRenderer::HighlightEffectsRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto vertex =
        compiler.Compile({
            .source = kFullscreenVs,
            .entryPoint = "main",
            .stage = shader::Stage::Vertex,
            .debug = false
        });

    const auto pixel =
        compiler.Compile({
            .source = kHighlightPs,
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
            .pushConstantDwords = 24U,
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

void HighlightEffectsRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& sourceHdr,
    rhi::Texture& targetDisplayLinear,
    const u32 width,
    const u32 height,
    const f32 exposureScale,
    const ToneMappingConfig& toneMapping,
    const HighlightEffectsConfig& config)
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
        DisplayHeadroomRatio(toneMapping);

    const std::array<u32, 24> constants{
        bits(std::max(exposureScale, 0.0F)),
        bits(toneMapping.enabled ? 1.0F : 0.0F),
        bits(headroom),
        bits(std::clamp(
            toneMapping.shoulderStart,
            0.0F,
            headroom)),

        bits(1.0F / static_cast<f32>(width)),
        bits(1.0F / static_cast<f32>(height)),
        bits(std::max(
            toneMapping.shoulderStrength,
            1.0e-3F)),
        0U,

        bits(config.bloomEnabled ? 1.0F : 0.0F),
        bits(std::max(config.bloomThreshold, 0.0F)),
        bits(std::max(config.bloomKnee, 1.0e-5F)),
        bits(std::max(config.bloomStrength, 0.0F)),

        bits(std::max(config.bloomRadiusPixels, 0.5F)),
        bits(config.glareEnabled ? 1.0F : 0.0F),
        bits(std::max(config.glareThreshold, 0.0F)),
        bits(std::max(config.glareStrength, 0.0F)),

        bits(std::max(config.glareRadiusPixels, 1.0F)),
        bits(config.flareEnabled ? 1.0F : 0.0F),
        bits(std::max(config.flareThreshold, 0.0F)),
        bits(std::max(config.flareStrength, 0.0F)),

        bits(std::max(config.flareCompactness, 1.0F)),
        bits(std::max(config.flareGhostScale, 0.0F)),
        bits(
            static_cast<f32>(
                static_cast<u8>(
                    config.debugMode))),
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
