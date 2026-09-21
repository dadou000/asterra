#include <orbit/post_process/ColorLut.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

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

constexpr const char* kColorLutPixelShader = R"(
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

[[vk::binding(1, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_lut;

[[vk::binding(1, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_lutSampler;

struct Constants
{
    float lutSize;
    float strength;
    float enabled;
    float padding;
};

[[vk::push_constant]]
Constants g;

float3 SamplePackedLut(float3 value)
{
    const float size = max(g.lutSize, 2.0);
    const float3 p = saturate(value) * (size - 1.0);

    const float blue0 = floor(p.b);
    const float blue1 = min(blue0 + 1.0, size - 1.0);
    const float blueWeight = p.b - blue0;

    const float width = size * size;
    const float2 uv0 = float2(
        (blue0 * size + p.r + 0.5) / width,
        (p.g + 0.5) / size);
    const float2 uv1 = float2(
        (blue1 * size + p.r + 0.5) / width,
        (p.g + 0.5) / size);

    const float3 c0 =
        g_lut.Sample(g_lutSampler, uv0).rgb;
    const float3 c1 =
        g_lut.Sample(g_lutSampler, uv1).rgb;

    return lerp(c0, c1, blueWeight);
}

float4 main(VSOutput input) : SV_Target0
{
    const float4 source =
        g_source.Sample(g_sourceSampler, input.uv);

    const float active =
        saturate(g.enabled) * saturate(g.strength);

    if (active <= 0.0)
        return source;

    const float3 corrected =
        SamplePackedLut(source.rgb);

    return float4(
        lerp(source.rgb, corrected, active),
        source.a);
}
)";
} // namespace

ColorLutData BuildIdentityColorLut(
    const u32 size)
{
    if (size < 2U || size > 128U)
    {
        throw std::invalid_argument(
            "Orbit color LUT size must be in [2, 128].");
    }

    ColorLutData result;
    result.size = size;
    result.rgba8.resize(
        static_cast<std::size_t>(size) *
        size * size * 4U);

    const f32 denominator =
        static_cast<f32>(size - 1U);

    for (u32 blue = 0U; blue < size; ++blue)
    {
        for (u32 green = 0U; green < size; ++green)
        {
            for (u32 red = 0U; red < size; ++red)
            {
                const u32 x =
                    blue * size + red;
                const u32 y = green;
                const std::size_t offset =
                    (static_cast<std::size_t>(y) *
                         size * size +
                     x) *
                    4U;

                const auto quantize =
                    [denominator](const u32 value)
                    {
                        return static_cast<u8>(
                            std::lround(
                                (static_cast<f32>(value) /
                                 denominator) *
                                255.0F));
                    };

                result.rgba8[offset] =
                    quantize(red);
                result.rgba8[offset + 1U] =
                    quantize(green);
                result.rgba8[offset + 2U] =
                    quantize(blue);
                result.rgba8[offset + 3U] =
                    255U;
            }
        }
    }

    return result;
}

GpuColorLut::GpuColorLut(
    rhi::Device& device,
    const ColorLutData& data)
    : size_(data.size)
{
    const std::size_t expectedBytes =
        static_cast<std::size_t>(size_) *
        size_ * size_ * 4U;

    if (size_ < 2U ||
        data.rgba8.size() != expectedBytes)
    {
        throw std::invalid_argument(
            "Orbit color LUT data is invalid.");
    }

    staging_ =
        device.CreateBuffer({
            .sizeBytes =
                static_cast<u64>(
                    data.rgba8.size()),
            .usage =
                rhi::BufferUsage::Generic,
            .memory =
                rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::CopySource
        });

    texture_ =
        device.CreateTexture({
            .width = size_ * size_,
            .height = size_,
            .format =
                rhi::TextureFormat::RGBA8_UNorm,
            .initialState =
                rhi::ResourceState::CopyDestination
        });

    if (!staging_ || !texture_)
    {
        throw std::runtime_error(
            "Failed to allocate Orbit color LUT resources.");
    }

    std::memcpy(
        staging_->Map(),
        data.rgba8.data(),
        data.rgba8.size());
    staging_->Unmap();
}

void GpuColorLut::EnsureUploaded(
    rhi::CommandList& commands)
{
    if (uploaded_)
    {
        return;
    }

    commands.CopyBufferToTexture(
        *staging_,
        0U,
        *texture_);
    commands.Transition(
        *texture_,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::ShaderResource);

    uploaded_ = true;
}

rhi::Texture& GpuColorLut::Texture() noexcept
{
    return *texture_;
}

u32 GpuColorLut::Size() const noexcept
{
    return size_;
}

ColorLutRenderer::ColorLutRenderer(
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
            .source = kColorLutPixelShader,
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
            .sampledTextures = 2U,
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

void ColorLutRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& source,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    GpuColorLut& lut,
    const ColorLutSettings& settings)
{
    if (width == 0U || height == 0U)
    {
        return;
    }

    lut.EnsureUploaded(commands);

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    const std::array<u32, 4> constants{
        bits(static_cast<f32>(lut.Size())),
        bits(std::clamp(
            settings.strength,
            0.0F,
            1.0F)),
        bits(settings.enabled ? 1.0F : 0.0F),
        0U
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
    commands.SetGraphicsTexture(1U, lut.Texture());
    commands.Draw(6U);
}
} // namespace orbit::post_process
