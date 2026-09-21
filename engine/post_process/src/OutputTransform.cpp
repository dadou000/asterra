#include <orbit/post_process/OutputTransform.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace orbit::post_process
{
namespace
{
constexpr f32 kPqM1 =
    2610.0F / 16384.0F;
constexpr f32 kPqM2 =
    2523.0F / 32.0F;
constexpr f32 kPqC1 =
    3424.0F / 4096.0F;
constexpr f32 kPqC2 =
    2413.0F / 128.0F;
constexpr f32 kPqC3 =
    2392.0F / 128.0F;
constexpr f32 kPqMaximumNits =
    10000.0F;

[[nodiscard]] f32 FiniteOr(
    const f32 value,
    const f32 fallback) noexcept
{
    return std::isfinite(value)
        ? value
        : fallback;
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

constexpr const char* kOutputPs = R"(
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
    float resolvedMode;
    float referenceWhiteNits;
    float resolvedPeakNits;
    float testPattern;
};
[[vk::push_constant]] Constants g;

float3 SrgbEncode(float3 x)
{
    x = max(x, 0.0);
    const float3 low =
        x * 12.92;
    const float3 high =
        1.055 * pow(x, 1.0 / 2.4) - 0.055;

    return lerp(
        low,
        high,
        step(0.0031308, x));
}

float3 PqEncodeNits(float3 nits)
{
    const float m1 =
        2610.0 / 16384.0;
    const float m2 =
        2523.0 / 32.0;
    const float c1 =
        3424.0 / 4096.0;
    const float c2 =
        2413.0 / 128.0;
    const float c3 =
        2392.0 / 128.0;

    const float3 normalized =
        saturate(
            max(nits, 0.0) /
            10000.0);
    const float3 p =
        pow(normalized, m1);

    return pow(
        (c1 + c2 * p) /
        (1.0 + c3 * p),
        m2);
}

float3 TestPattern(
    float2 uv)
{
    if (g.testPattern > 0.5 &&
        g.testPattern < 1.5)
    {
        const float value =
            saturate(uv.x);
        return value.xxx;
    }

    if (g.testPattern >= 1.5 &&
        g.testPattern < 2.5)
    {
        return float3(1.0, 1.0, 1.0);
    }

    if (g.testPattern >= 2.5)
    {
        const float relativePeak =
            g.resolvedPeakNits /
            max(g.referenceWhiteNits, 1.0e-3);
        return float3(relativePeak, relativePeak, relativePeak);
    }

    return float3(-1.0, -1.0, -1.0);
}

float4 main(VSOutput input) : SV_Target0
{
    float3 displayLinear =
        max(
            g_source.Sample(
                g_sourceSampler,
                input.uv).rgb,
            0.0);

    const float3 test =
        TestPattern(input.uv);

    if (test.x >= 0.0)
    {
        displayLinear = test;
    }

    if (g.resolvedMode > 1.5)
    {
        const float3 absoluteNits =
            min(
                displayLinear *
                    max(
                        g.referenceWhiteNits,
                        1.0e-3),
                max(
                    g.resolvedPeakNits,
                    g.referenceWhiteNits));

        return float4(
            PqEncodeNits(
                absoluteNits),
            1.0);
    }

    return float4(
        SrgbEncode(
            saturate(displayLinear)),
        1.0);
}
)";
} // namespace

OutputTransformDiagnostics
ResolveOutputTransform(
    const OutputTransformSettings& settings,
    const OutputDisplayCapabilities& capabilities) noexcept
{
    OutputTransformDiagnostics result{};

    result.requestedMode =
        settings.mode;
    result.hdrCapabilityAvailable =
        capabilities.hdr10Supported;
    result.referenceWhiteNits =
        std::max(
            FiniteOr(
                settings.referenceWhiteNits,
                203.0F),
            1.0F);
    result.requestedPeakNits =
        std::max(
            FiniteOr(
                settings.requestedPeakNits,
                1000.0F),
            result.referenceWhiteNits);

    const bool wantsHdr =
        settings.mode ==
            OutputMode::Hdr10 ||
        (settings.mode ==
             OutputMode::Auto &&
         capabilities.hdr10Supported);

    if (wantsHdr &&
        capabilities.hdr10Supported)
    {
        result.resolvedMode =
            OutputMode::Hdr10;

        const f32 reportedPeak =
            FiniteOr(
                capabilities.reportedPeakNits,
                0.0F);

        result.resolvedPeakNits =
            reportedPeak >
                    result.referenceWhiteNits
                ? std::min(
                      result.requestedPeakNits,
                      reportedPeak)
                : result.requestedPeakNits;
    }
    else
    {
        result.resolvedMode =
            OutputMode::Sdr;
        result.fellBackToSdr =
            settings.mode ==
                OutputMode::Hdr10;
        result.resolvedPeakNits =
            result.referenceWhiteNits;
    }

    return result;
}

f32 EncodeSrgbChannel(
    const f32 linear) noexcept
{
    const f32 value =
        std::max(
            FiniteOr(linear, 0.0F),
            0.0F);

    if (value <= 0.0031308F)
    {
        return 12.92F * value;
    }

    return
        1.055F *
            std::pow(
                value,
                1.0F / 2.4F) -
        0.055F;
}

f32 EncodeSt2084FromNits(
    const f32 nits) noexcept
{
    const f32 normalized =
        std::clamp(
            FiniteOr(nits, 0.0F) /
                kPqMaximumNits,
            0.0F,
            1.0F);

    const f32 p =
        std::pow(
            normalized,
            kPqM1);

    return std::pow(
        (kPqC1 + kPqC2 * p) /
            (1.0F + kPqC3 * p),
        kPqM2);
}

f32 DecodeSt2084ToNits(
    const f32 encoded) noexcept
{
    const f32 e =
        std::clamp(
            FiniteOr(encoded, 0.0F),
            0.0F,
            1.0F);

    const f32 p =
        std::pow(
            e,
            1.0F / kPqM2);

    const f32 numerator =
        std::max(
            p - kPqC1,
            0.0F);
    const f32 denominator =
        std::max(
            kPqC2 -
                kPqC3 * p,
            1.0e-8F);

    return
        std::pow(
            numerator /
                denominator,
            1.0F / kPqM1) *
        kPqMaximumNits;
}

std::string_view OutputModeName(
    const OutputMode mode) noexcept
{
    switch (mode)
    {
    case OutputMode::Auto:
        return "Auto";
    case OutputMode::Sdr:
        return "SDR";
    case OutputMode::Hdr10:
        return "HDR10 / PQ";
    }

    return "Unknown";
}

std::string_view OutputTestPatternName(
    const OutputTestPattern pattern) noexcept
{
    switch (pattern)
    {
    case OutputTestPattern::None:
        return "Off";
    case OutputTestPattern::LinearRamp:
        return "Linear Ramp";
    case OutputTestPattern::ReferenceWhite:
        return "Reference White";
    case OutputTestPattern::PeakWhite:
        return "Peak White";
    }

    return "Unknown";
}

OutputTransformRenderer::OutputTransformRenderer(
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
            .source = kOutputPs,
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
            .depthWrite = false,
            .colorAttachmentFormats = {
                rhi::TextureFormat::RGBA8_UNorm
            },
            .colorAttachmentCount = 1U
        });
}

void OutputTransformRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& sourceDisplayLinear,
    rhi::Texture& targetEncoded,
    const u32 width,
    const u32 height,
    const OutputTransformDiagnostics& diagnostics,
    const OutputTestPattern pattern)
{
    if (width == 0U ||
        height == 0U)
    {
        return;
    }

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(
                value);
        };

    const std::array<u32, 4> constants{
        bits(
            static_cast<f32>(
                static_cast<u8>(
                    diagnostics.resolvedMode))),
        bits(
            diagnostics.referenceWhiteNits),
        bits(
            diagnostics.resolvedPeakNits),
        bits(
            static_cast<f32>(
                static_cast<u8>(
                    pattern)))
    };

    commands.SetRenderTarget(
        targetEncoded);
    commands.SetViewport({
        .x = 0.0F,
        .y = 0.0F,
        .width =
            static_cast<f32>(width),
        .height =
            static_cast<f32>(height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F
    });
    commands.SetScissor({
        .left = 0,
        .top = 0,
        .right =
            static_cast<i32>(width),
        .bottom =
            static_cast<i32>(height)
    });

    commands.SetGraphicsPipeline(
        *pipeline_);
    commands.SetGraphicsConstants(
        constants);
    commands.SetGraphicsTexture(
        0U,
        sourceDisplayLinear);
    commands.Draw(6U);
}
} // namespace orbit::post_process
