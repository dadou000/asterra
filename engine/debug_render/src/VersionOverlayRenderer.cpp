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
        config.marginPixels +
        config.extraTopMarginPixels;

    result[6] =
        config.paddingPixels;

    result[7] =
        scale;

    result[8] =
        static_cast<u32>(
            text.size());

    result[9] =
        config.anchor ==
                OverlayAnchor::TopLeft
            ? 1U
            : 0U;

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
struct OverlayConstants
{
    uint4 g_metrics0;
    uint4 g_metrics1;
    uint4 g_metrics2;
    uint4 g_text0;
    uint4 g_text1;
};
[[vk::push_constant]] OverlayConstants g_pc;

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
            (float)g_pc.g_metrics0.x,
            (float)g_pc.g_metrics0.y);

    const float2 panelSize =
        float2(
            (float)g_pc.g_metrics0.z,
            (float)g_pc.g_metrics0.w);

    const float2 margin =
        float2(
            (float)g_pc.g_metrics1.x,
            (float)g_pc.g_metrics1.y);

    const bool anchorTopLeft =
        g_pc.g_metrics2.y != 0u;

    const float2 panelTopLeft =
        anchorTopLeft
            ? margin
            : targetSize -
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
struct OverlayConstants
{
    uint4 g_metrics0;
    uint4 g_metrics1;
    uint4 g_metrics2;
    uint4 g_text0;
    uint4 g_text1;
};
[[vk::push_constant]] OverlayConstants g_pc;

struct VSOutput
{
    float4 position : SV_Position;
    float2 localPixel : TEXCOORD0;
};

uint PackedTextWord(uint index)
{
    uint word = g_pc.g_text1.w;
    if (index == 0u) word = g_pc.g_text0.x;
    else if (index == 1u) word = g_pc.g_text0.y;
    else if (index == 2u) word = g_pc.g_text0.z;
    else if (index == 3u) word = g_pc.g_text0.w;
    else if (index == 4u) word = g_pc.g_text1.x;
    else if (index == 5u) word = g_pc.g_text1.y;
    else if (index == 6u) word = g_pc.g_text1.z;
    return word;
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
    uint bits = 0u;

    if (character == 48u) bits = 0x699996u;
    else if (character == 49u) bits = 0x722262u;
    else if (character == 50u) bits = 0xF42196u;
    else if (character == 51u) bits = 0x69161Eu;
    else if (character == 52u) bits = 0x22FA62u;
    else if (character == 53u) bits = 0x691E8Fu;
    else if (character == 54u) bits = 0x699E86u;
    else if (character == 55u) bits = 0x44421Fu;
    else if (character == 56u) bits = 0x699696u;
    else if (character == 57u) bits = 0x617996u;

    else if (character == 65u) bits = 0x99F996u;
    else if (character == 66u) bits = 0xE99E9Eu;
    else if (character == 67u) bits = 0x788887u;
    else if (character == 68u) bits = 0xE9999Eu;
    else if (character == 69u) bits = 0xF88E8Fu;
    else if (character == 70u) bits = 0x888E8Fu;
    else if (character == 71u) bits = 0x799B87u;
    else if (character == 72u) bits = 0x999F99u;
    else if (character == 73u) bits = 0xF2222Fu;
    else if (character == 74u) bits = 0x699111u;
    else if (character == 75u) bits = 0x99ACA9u;
    else if (character == 76u) bits = 0xF88888u;
    else if (character == 77u) bits = 0x999FF9u;
    else if (character == 78u) bits = 0x999BD9u;
    else if (character == 79u) bits = 0x699996u;
    else if (character == 80u) bits = 0x88E99Eu;
    else if (character == 81u) bits = 0x7B9996u;
    else if (character == 82u) bits = 0x9AE99Eu;
    else if (character == 83u) bits = 0xE11687u;
    else if (character == 84u) bits = 0x22222Fu;
    else if (character == 85u) bits = 0x699999u;
    else if (character == 86u) bits = 0x669999u;
    else if (character == 87u) bits = 0x9FF999u;
    else if (character == 88u) bits = 0x996699u;
    else if (character == 89u) bits = 0x222699u;
    else if (character == 90u) bits = 0xF8421Fu;

    else if (character == 46u) bits = 0x200000u;
    else if (character == 45u) bits = 0x000F00u;
    else if (character == 58u) bits = 0x020020u;

    return bits;
}

bool GlyphPixel(
    uint character,
    uint column,
    uint row)
{
    bool lit = false;

    if (column < 4u &&
        row < 6u)
    {
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

        lit = (rowBits & mask) != 0u;
    }

    return lit;
}

float4 main(VSOutput input) : SV_Target0
{
    const float2 panelSize =
        float2(
            (float)g_pc.g_metrics0.z,
            (float)g_pc.g_metrics0.w);

    const float padding =
        (float)g_pc.g_metrics1.z;

    const float scale =
        max(
            (float)g_pc.g_metrics1.w,
            1.0);

    const uint textLength =
        g_pc.g_metrics2.x;

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

    void SetText(
        const std::string_view text)
    {
        text_ = SanitizeText(text);
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

void VersionOverlayRenderer::SetText(
    const std::string_view text)
{
    impl_->SetText(text);
}
} // namespace orbit::debug_render
