#include <orbit/debug_render/VersionOverlayRenderer.hpp>

#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/rhi/Resource.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace orbit::debug_render
{
namespace
{
constexpr u32 kMaximumCharacters = 32;
constexpr u32 kGlyphWidth = 4;
constexpr u32 kGlyphHeight = 6;
constexpr u32 kGlyphAdvance = 5;
constexpr u32 kConstantDwords = 20;

[[nodiscard]] std::string SanitizeText(
    const std::string_view text)
{
    std::string result;
    result.reserve(
        (std::min)(
            text.size(),
            static_cast<std::size_t>(
                kMaximumCharacters)));

    for (const char character :
         text)
    {
        if (result.size() >=
            kMaximumCharacters)
        {
            break;
        }

        const unsigned char value =
            static_cast<unsigned char>(
                character);

        if (value >= 'a' &&
            value <= 'z')
        {
            result.push_back(
                static_cast<char>(
                    std::toupper(
                        value)));

            continue;
        }

        const bool supported =
            (value >= 'A' &&
             value <= 'Z') ||
            (value >= '0' &&
             value <= '9') ||
            value == ' ' ||
            value == '.' ||
            value == '-' ||
            value == ':';

        result.push_back(
            supported
                ? static_cast<char>(
                    value)
                : '-');
    }

    if (result.empty())
    {
        result = "ORBIT";
    }

    return result;
}

void UploadBuffer(
    rhi::Buffer& buffer,
    const void* source,
    const std::size_t bytes)
{
    std::byte* destination =
        buffer.Map();

    std::memcpy(
        destination,
        source,
        bytes);

    buffer.Unmap();
}

[[nodiscard]] std::array<u32, kConstantDwords>
BuildConstants(
    const std::string_view text,
    const VersionOverlayConfig& config,
    const u32 targetWidth,
    const u32 targetHeight)
{
    std::array<u32, kConstantDwords>
        result{};

    const u32 scale =
        (std::max)(
            config.pixelScale,
            1U);

    const u32 textWidth =
        static_cast<u32>(
            text.size()) *
            kGlyphAdvance *
            scale -
        scale;

    const u32 panelWidth =
        textWidth +
        config.paddingPixels *
            2U;

    const u32 panelHeight =
        kGlyphHeight *
            scale +
        config.paddingPixels *
            2U;

    result[0] = targetWidth;
    result[1] = targetHeight;
    result[2] = panelWidth;
    result[3] = panelHeight;

    result[4] =
        config.marginPixels;

    result[5] =
        config.marginPixels;

    result[6] =
        config.paddingPixels;

    result[7] =
        scale;

    result[8] =
        static_cast<u32>(
            text.size());

    for (u32 index = 0;
         index <
            static_cast<u32>(
                text.size());
         ++index)
    {
        const u32 word =
            index / 4U;

        const u32 shift =
            (index % 4U) *
            8U;

        result[12U + word] |=
            static_cast<u32>(
                static_cast<u8>(
                    text[index])) <<
            shift;
    }

    return result;
}

constexpr const char* kVertexShader = R"(
cbuffer OverlayConstants : register(b0)
{
    uint4 g_metrics0;
    uint4 g_metrics1;
    uint4 g_metrics2;
    uint4 g_text0;
    uint4 g_text1;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 localPixel : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    const float2 corners[4] =
    {
        float2(0.0, 0.0),
        float2(1.0, 0.0),
        float2(1.0, 1.0),
        float2(0.0, 1.0)
    };

    const float2 targetSize =
        float2(
            (float)g_metrics0.x,
            (float)g_metrics0.y);

    const float2 panelSize =
        float2(
            (float)g_metrics0.z,
            (float)g_metrics0.w);

    const float2 margin =
        float2(
            (float)g_metrics1.x,
            (float)g_metrics1.y);

    const float2 panelTopLeft =
        targetSize -
        panelSize -
        margin;

    const float2 local =
        corners[vertexId] *
        panelSize;

    const float2 pixel =
        panelTopLeft +
        local;

    const float2 ndc =
        float2(
            pixel.x /
                max(
                    targetSize.x,
                    1.0) *
                2.0 -
                1.0,
            1.0 -
                pixel.y /
                max(
                    targetSize.y,
                    1.0) *
                2.0);

    VSOutput output;

    output.position =
        float4(
            ndc,
            0.0,
            1.0);

    output.localPixel =
        local;

    return output;
}
)";

constexpr const char* kPixelShader = R"(
cbuffer OverlayConstants : register(b0)
{
    uint4 g_metrics0;
    uint4 g_metrics1;
    uint4 g_metrics2;
    uint4 g_text0;
    uint4 g_text1;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 localPixel : TEXCOORD0;
};

uint PackedTextWord(uint index)
{
    if (index == 0u) return g_text0.x;
    if (index == 1u) return g_text0.y;
    if (index == 2u) return g_text0.z;
    if (index == 3u) return g_text0.w;
    if (index == 4u) return g_text1.x;
    if (index == 5u) return g_text1.y;
    if (index == 6u) return g_text1.z;
    return g_text1.w;
}

uint CharacterAt(uint index)
{
    const uint word =
        PackedTextWord(
            index / 4u);

    const uint shift =
        (index & 3u) *
        8u;

    return
        (word >> shift) &
        0xFFu;
}

uint Glyph(uint character)
{
    if (character >= 48u &&
        character <= 57u)
    {
        switch (character)
        {
        case 48u: return 0x699996u;
        case 49u: return 0x722262u;
        case 50u: return 0xF42196u;
        case 51u: return 0x69161Eu;
        case 52u: return 0x22FA62u;
        case 53u: return 0x691E8Fu;
        case 54u: return 0x699E86u;
        case 55u: return 0x44421Fu;
        case 56u: return 0x699696u;
        case 57u: return 0x617996u;
        }
    }

    switch (character)
    {
    case 65u: return 0x99F996u;
    case 66u: return 0xE99E9Eu;
    case 67u: return 0x788887u;
    case 68u: return 0xE9999Eu;
    case 69u: return 0xF88E8Fu;
    case 70u: return 0x888E8Fu;
    case 71u: return 0x799B87u;
    case 72u: return 0x999F99u;
    case 73u: return 0xF2222Fu;
    case 74u: return 0x699111u;
    case 75u: return 0x99ACA9u;
    case 76u: return 0xF88888u;
    case 77u: return 0x999FF9u;
    case 78u: return 0x999BD9u;
    case 79u: return 0x699996u;
    case 80u: return 0x88E99Eu;
    case 81u: return 0x7B9996u;
    case 82u: return 0x9AE99Eu;
    case 83u: return 0xE11687u;
    case 84u: return 0x22222Fu;
    case 85u: return 0x699999u;
    case 86u: return 0x669999u;
    case 87u: return 0x9FF999u;
    case 88u: return 0x996699u;
    case 89u: return 0x222699u;
    case 90u: return 0xF8421Fu;

    case 46u: return 0x200000u;
    case 45u: return 0x000F00u;
    case 58u: return 0x020020u;
    default: return 0u;
    }
}

bool GlyphPixel(
    uint character,
    uint column,
    uint row)
{
    if (column >= 4u ||
        row >= 6u)
    {
        return false;
    }

    const uint glyph =
        Glyph(
            character);

    const uint rowBits =
        (glyph >>
         (row * 4u)) &
        0xFu;

    const uint mask =
        1u <<
        (3u - column);

    return
        (rowBits & mask) != 0u;
}

float4 main(VSOutput input) : SV_Target0
{
    const float2 panelSize =
        float2(
            (float)g_metrics0.z,
            (float)g_metrics0.w);

    const float padding =
        (float)g_metrics1.z;

    const float scale =
        max(
            (float)g_metrics1.w,
            1.0);

    const uint textLength =
        g_metrics2.x;

    const float2 local =
        input.localPixel;

    const bool border =
        local.x < 1.5 ||
        local.y < 1.5 ||
        local.x >
            panelSize.x - 1.5 ||
        local.y >
            panelSize.y - 1.5;

    const float3 background =
        float3(
            0.012,
            0.020,
            0.024);

    const float3 borderColor =
        float3(
            0.12,
            0.34,
            0.38);

    const float3 textColor =
        float3(
            0.72,
            0.94,
            0.96);

    if (border)
    {
        return float4(
            borderColor,
            1.0);
    }

    const float2 textPixel =
        local -
        padding;

    if (textPixel.x < 0.0 ||
        textPixel.y < 0.0)
    {
        return float4(
            background,
            1.0);
    }

    const uint advance =
        (uint)(
            5.0 * scale);

    const uint characterIndex =
        (uint)textPixel.x /
        max(
            advance,
            1u);

    if (characterIndex >=
        textLength)
    {
        return float4(
            background,
            1.0);
    }

    const uint characterLocalX =
        (uint)textPixel.x %
        max(
            advance,
            1u);

    const uint glyphWidth =
        (uint)(
            4.0 * scale);

    const uint glyphHeight =
        (uint)(
            6.0 * scale);

    if (characterLocalX >=
            glyphWidth ||
        textPixel.y >=
            (float)glyphHeight)
    {
        return float4(
            background,
            1.0);
    }

    const uint glyphX =
        characterLocalX /
        (uint)scale;

    const uint glyphY =
        (uint)textPixel.y /
        (uint)scale;

    const uint character =
        CharacterAt(
            characterIndex);

    if (GlyphPixel(
            character,
            glyphX,
            glyphY))
    {
        return float4(
            textColor,
            1.0);
    }

    return float4(
        background,
        1.0);
}
)";
} // namespace

class VersionOverlayRenderer::Impl
{
public:
    Impl(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        const std::string_view text,
        VersionOverlayConfig config)
        : text_(SanitizeText(text)),
          config_(config)
    {
        if (config_.pixelScale == 0)
        {
            throw std::invalid_argument(
                "Orbit version overlay pixel scale must be non-zero.");
        }

        const shader::Binary
            vertexShader =
                shaderCompiler.Compile({
                    .source =
                        kVertexShader,
                    .entryPoint =
                        "main",
                    .stage =
                        shader::Stage::Vertex,
                    .debug = false
                });

        const shader::Binary
            pixelShader =
                shaderCompiler.Compile({
                    .source =
                        kPixelShader,
                    .entryPoint =
                        "main",
                    .stage =
                        shader::Stage::Pixel,
                    .debug = false
                });

        pipeline_ =
            device.CreateGraphicsPipeline({
                .vertexShader = {
                    .data =
                        vertexShader.
                            bytecode.
                            data(),
                    .size =
                        vertexShader.
                            bytecode.
                            size()
                },
                .pixelShader = {
                    .data =
                        pixelShader.
                            bytecode.
                            data(),
                    .size =
                        pixelShader.
                            bytecode.
                            size()
                },
                .vertexAttributes = {},
                .vertexStrideBytes = 0,
                .pushConstantDwords =
                    kConstantDwords,
                .shaderResourceBuffers = 0,
                .topology =
                    rhi::PrimitiveTopology::
                        TriangleList,
                .fillMode =
                    rhi::FillMode::Solid,
                .cullMode =
                    rhi::CullMode::None,
                .depthTest = false,
                .depthWrite = false
            });

        constexpr std::array<u32, 6>
            indices{
                0U,
                1U,
                2U,
                0U,
                2U,
                3U
            };

        indexBuffer_ =
            device.CreateBuffer({
                .sizeBytes =
                    sizeof(indices),
                .usage =
                    rhi::BufferUsage::Index,
                .memory =
                    rhi::MemoryUsage::
                        HostVisible,
                .initialState =
                    rhi::ResourceState::
                        IndexBuffer
            });

        UploadBuffer(
            *indexBuffer_,
            indices.data(),
            sizeof(indices));
    }

    void Draw(
        rhi::CommandList& commandList,
        rhi::Texture& target,
        const u32 targetWidth,
        const u32 targetHeight)
    {
        if (targetWidth == 0 ||
            targetHeight == 0)
        {
            return;
        }

        const auto constants =
            BuildConstants(
                text_,
                config_,
                targetWidth,
                targetHeight);

        commandList.SetRenderTarget(
            target);

        commandList.SetViewport({
            .x = 0.0F,
            .y = 0.0F,
            .width =
                static_cast<f32>(
                    targetWidth),
            .height =
                static_cast<f32>(
                    targetHeight),
            .minDepth = 0.0F,
            .maxDepth = 1.0F
        });

        commandList.SetScissor({
            .left = 0,
            .top = 0,
            .right =
                static_cast<i32>(
                    targetWidth),
            .bottom =
                static_cast<i32>(
                    targetHeight)
        });

        commandList.SetGraphicsPipeline(
            *pipeline_);

        commandList.SetGraphicsConstants(
            constants);

        commandList.SetIndexBuffer(
            *indexBuffer_,
            rhi::IndexFormat::UInt32);

        commandList.DrawIndexed(6);
    }

    [[nodiscard]] std::string_view Text()
        const noexcept
    {
        return text_;
    }

private:
    std::string text_;
    VersionOverlayConfig config_{};

    std::unique_ptr<rhi::GraphicsPipeline>
        pipeline_;

    std::unique_ptr<rhi::Buffer>
        indexBuffer_;
};

VersionOverlayRenderer::
VersionOverlayRenderer(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const std::string_view text,
    const VersionOverlayConfig config)
    : impl_(
        std::make_unique<Impl>(
            device,
            shaderCompiler,
            text,
            config))
{
}

VersionOverlayRenderer::
~VersionOverlayRenderer() = default;

void VersionOverlayRenderer::Draw(
    rhi::CommandList& commandList,
    rhi::Texture& target,
    const u32 targetWidth,
    const u32 targetHeight)
{
    impl_->Draw(
        commandList,
        target,
        targetWidth,
        targetHeight);
}

std::string_view
VersionOverlayRenderer::Text()
    const noexcept
{
    return impl_->Text();
}
} // namespace orbit::debug_render
