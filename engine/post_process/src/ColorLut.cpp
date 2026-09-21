#include <orbit/post_process/ColorLut.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <string>
#include <string_view>

namespace orbit::post_process
{
namespace
{
[[nodiscard]] std::string Trim(
    std::string value)
{
    const auto first =
        value.find_first_not_of(" \t\r\n");

    if (first == std::string::npos)
    {
        return {};
    }

    const auto last =
        value.find_last_not_of(" \t\r\n");

    return value.substr(
        first,
        last - first + 1U);
}

[[nodiscard]] bool StartsWith(
    const std::string_view text,
    const std::string_view prefix) noexcept
{
    return text.size() >= prefix.size() &&
        text.substr(0U, prefix.size()) == prefix;
}

[[nodiscard]] std::string Unquote(
    std::string value)
{
    value = Trim(std::move(value));

    if (value.size() >= 2U &&
        value.front() == '"' &&
        value.back() == '"')
    {
        value =
            value.substr(
                1U,
                value.size() - 2U);
    }

    return value;
}

[[nodiscard]] f32 ParseFiniteFloat(
    const std::string& text,
    const char* const label)
{
    std::size_t consumed = 0U;
    const f32 value =
        std::stof(
            text,
            &consumed);

    if (consumed != text.size() ||
        !std::isfinite(value))
    {
        throw std::invalid_argument(
            std::string("Invalid ") +
            label +
            " value in .cube LUT.");
    }

    return value;
}

[[nodiscard]] u32 ParseSize(
    const std::string& text)
{
    std::size_t consumed = 0U;
    const unsigned long value =
        std::stoul(
            text,
            &consumed);

    if (consumed != text.size() ||
        value < 2UL ||
        value > 128UL)
    {
        throw std::invalid_argument(
            "LUT_3D_SIZE must be in [2, 128].");
    }

    return static_cast<u32>(value);
}

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
    float domainMinimum;
    float domainMaximum;
    float3 padding;
};

[[vk::push_constant]]
Constants g;

float3 SamplePackedLut(float3 value)
{
    const float size = max(g.lutSize, 2.0);
    const float domainRange =
        max(
            g.domainMaximum -
                g.domainMinimum,
            1.0e-6);
    const float3 normalized =
        saturate(
            (value -
             g.domainMinimum) /
            domainRange);
    const float3 p =
        normalized *
        (size - 1.0);

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

    float3 displayLinear =
        max(source.rgb, 0.0);

    const float active =
        saturate(g.enabled) *
        saturate(g.strength);

    const bool inDomain =
        all(
            displayLinear >=
                float3(
                    g.domainMinimum,
                    g.domainMinimum,
                    g.domainMinimum)) &&
        all(
            displayLinear <=
                float3(
                    g.domainMaximum,
                    g.domainMaximum,
                    g.domainMaximum));

    if (active > 0.0 &&
        inDomain)
    {
        const float3 corrected =
            SamplePackedLut(displayLinear);

        displayLinear =
            lerp(
                displayLinear,
                corrected,
                active);
    }

    return float4(
        displayLinear,
        source.a);
}
)";
} // namespace

std::string_view
ColorLutDomainName(
    const ColorLutDomain domain) noexcept
{
    switch (domain)
    {
    case ColorLutDomain::DisplayLinear:
        return "Display Linear";
    case ColorLutDomain::ShapedSceneLinear:
        return "Shaped Scene Linear";
    }

    return "Unknown";
}

std::string_view
ColorLutShaperName(
    const ColorLutShaper shaper) noexcept
{
    switch (shaper)
    {
    case ColorLutShaper::None:
        return "None";
    case ColorLutShaper::Log2:
        return "Log2";
    }

    return "Unknown";
}

f32 BlendColorLutChannel(
    const f32 source,
    const f32 corrected,
    const f32 strength) noexcept
{
    const f32 active =
        std::clamp(
            strength,
            0.0F,
            1.0F);

    return
        source +
        (corrected - source) *
            active;
}

bool IsDisplayLutCompatible(
    const ColorLutData& lut) noexcept
{
    return
        lut.metadata.domain ==
            ColorLutDomain::DisplayLinear &&
        lut.metadata.shaper ==
            ColorLutShaper::None &&
        std::isfinite(
            lut.metadata.domainMinimum) &&
        std::isfinite(
            lut.metadata.domainMaximum) &&
        lut.metadata.domainMaximum >
            lut.metadata.domainMinimum;
}

ColorLutImportResult
ParseCubeColorLut(
    const std::string_view text)
{
    ColorLutImportResult result{};
    result.lut.metadata =
        ColorLutMetadata{};

    std::istringstream stream{
        std::string(text)};

    std::string line;
    u32 declaredSize = 0U;
    std::vector<std::array<f32, 3>>
        samples;

    while (std::getline(stream, line))
    {
        line = Trim(std::move(line));

        if (line.empty())
        {
            continue;
        }

        if (StartsWith(
                line,
                "# ORBIT_DOMAIN "))
        {
            const std::string value =
                Trim(
                    line.substr(
                        std::string(
                            "# ORBIT_DOMAIN ").
                            size()));

            if (value == "DISPLAY_LINEAR")
            {
                result.lut.metadata.domain =
                    ColorLutDomain::
                        DisplayLinear;
            }
            else if (value ==
                     "SHAPED_SCENE_LINEAR")
            {
                result.lut.metadata.domain =
                    ColorLutDomain::
                        ShapedSceneLinear;
            }
            else
            {
                throw std::invalid_argument(
                    "Unsupported ORBIT_DOMAIN in .cube LUT.");
            }

            result.lut.metadata.
                explicitOrbitMetadata = true;
            continue;
        }

        if (StartsWith(
                line,
                "# ORBIT_SHAPER "))
        {
            const std::string value =
                Trim(
                    line.substr(
                        std::string(
                            "# ORBIT_SHAPER ").
                            size()));

            if (value == "NONE")
            {
                result.lut.metadata.shaper =
                    ColorLutShaper::None;
            }
            else if (value == "LOG2")
            {
                result.lut.metadata.shaper =
                    ColorLutShaper::Log2;
            }
            else
            {
                throw std::invalid_argument(
                    "Unsupported ORBIT_SHAPER in .cube LUT.");
            }

            result.lut.metadata.
                explicitOrbitMetadata = true;
            continue;
        }

        if (line.front() == '#')
        {
            continue;
        }

        if (StartsWith(line, "TITLE "))
        {
            result.lut.metadata.title =
                Unquote(
                    line.substr(6U));
            continue;
        }

        if (StartsWith(
                line,
                "LUT_3D_SIZE "))
        {
            declaredSize =
                ParseSize(
                    Trim(
                        line.substr(12U)));
            continue;
        }

        if (StartsWith(
                line,
                "DOMAIN_MIN "))
        {
            std::istringstream values{
                line.substr(11U)};
            std::string x;
            std::string y;
            std::string z;
            values >> x >> y >> z;

            if (x.empty() ||
                y.empty() ||
                z.empty())
            {
                throw std::invalid_argument(
                    "DOMAIN_MIN requires three values.");
            }

            const f32 px =
                ParseFiniteFloat(x, "DOMAIN_MIN");
            const f32 py =
                ParseFiniteFloat(y, "DOMAIN_MIN");
            const f32 pz =
                ParseFiniteFloat(z, "DOMAIN_MIN");

            if (std::abs(px - py) > 1.0e-6F ||
                std::abs(px - pz) > 1.0e-6F)
            {
                throw std::invalid_argument(
                    "Orbit currently requires uniform .cube DOMAIN_MIN.");
            }

            result.lut.metadata.domainMinimum =
                px;
            continue;
        }

        if (StartsWith(
                line,
                "DOMAIN_MAX "))
        {
            std::istringstream values{
                line.substr(11U)};
            std::string x;
            std::string y;
            std::string z;
            values >> x >> y >> z;

            if (x.empty() ||
                y.empty() ||
                z.empty())
            {
                throw std::invalid_argument(
                    "DOMAIN_MAX requires three values.");
            }

            const f32 px =
                ParseFiniteFloat(x, "DOMAIN_MAX");
            const f32 py =
                ParseFiniteFloat(y, "DOMAIN_MAX");
            const f32 pz =
                ParseFiniteFloat(z, "DOMAIN_MAX");

            if (std::abs(px - py) > 1.0e-6F ||
                std::abs(px - pz) > 1.0e-6F)
            {
                throw std::invalid_argument(
                    "Orbit currently requires uniform .cube DOMAIN_MAX.");
            }

            result.lut.metadata.domainMaximum =
                px;
            continue;
        }

        if (StartsWith(
                line,
                "LUT_1D_SIZE "))
        {
            throw std::invalid_argument(
                "1D .cube LUTs are not supported by the M28 3D color pipeline.");
        }

        std::istringstream values{
            line};
        std::string r;
        std::string g;
        std::string b;
        values >> r >> g >> b;

        if (r.empty() ||
            g.empty() ||
            b.empty())
        {
            throw std::invalid_argument(
                "Unrecognized .cube LUT statement.");
        }

        samples.push_back({
            ParseFiniteFloat(r, "LUT sample"),
            ParseFiniteFloat(g, "LUT sample"),
            ParseFiniteFloat(b, "LUT sample")
        });
    }

    if (declaredSize == 0U)
    {
        throw std::invalid_argument(
            ".cube LUT is missing LUT_3D_SIZE.");
    }

    const std::size_t expectedSamples =
        static_cast<std::size_t>(
            declaredSize) *
        declaredSize *
        declaredSize;

    if (samples.size() !=
        expectedSamples)
    {
        throw std::invalid_argument(
            ".cube LUT sample count does not match LUT_3D_SIZE.");
    }

    if (!(result.lut.metadata.
              domainMaximum >
          result.lut.metadata.
              domainMinimum))
    {
        throw std::invalid_argument(
            ".cube LUT DOMAIN_MAX must be greater than DOMAIN_MIN.");
    }

    result.lut.size =
        declaredSize;
    result.lut.rgba8.resize(
        expectedSamples * 4U);

    // .cube rows are R-fastest, then G, then B. Orbit packs blue
    // slices horizontally and green along texture Y, so the semantic axes
    // match but row-major byte order requires an explicit remap.
    for (u32 blue = 0U;
         blue < declaredSize;
         ++blue)
    {
        for (u32 green = 0U;
             green < declaredSize;
             ++green)
        {
            for (u32 red = 0U;
                 red < declaredSize;
                 ++red)
            {
                const std::size_t cubeIndex =
                    (static_cast<std::size_t>(blue) *
                         declaredSize *
                         declaredSize) +
                    (static_cast<std::size_t>(green) *
                         declaredSize) +
                    red;

                const u32 packedX =
                    blue * declaredSize +
                    red;
                const u32 packedY =
                    green;
                const std::size_t packedOffset =
                    (static_cast<std::size_t>(packedY) *
                         declaredSize *
                         declaredSize +
                     packedX) *
                    4U;

                for (u32 channel = 0U;
                     channel < 3U;
                     ++channel)
                {
                    const f32 value =
                        std::clamp(
                            samples[cubeIndex][channel],
                            0.0F,
                            1.0F);

                    result.lut.rgba8[
                        packedOffset +
                        channel] =
                        static_cast<u8>(
                            std::lround(
                                value *
                                255.0F));
                }

                result.lut.rgba8[
                    packedOffset + 3U] =
                        255U;
            }
        }
    }

    result.canonicalCube =
        SerializeCubeColorLut(
            result.lut);

    return result;
}

std::string SerializeCubeColorLut(
    const ColorLutData& lut)
{
    const std::size_t expectedBytes =
        static_cast<std::size_t>(
            lut.size) *
        lut.size *
        lut.size *
        4U;

    if (lut.size < 2U ||
        lut.size > 128U ||
        lut.rgba8.size() !=
            expectedBytes)
    {
        throw std::invalid_argument(
            "Cannot serialize invalid Orbit color LUT data.");
    }

    std::ostringstream output;
    output.setf(
        std::ios::fixed,
        std::ios::floatfield);
    output <<
        std::setprecision(9);

    output
        << "# ORBIT_DOMAIN "
        << (lut.metadata.domain ==
                    ColorLutDomain::DisplayLinear
                ? "DISPLAY_LINEAR"
                : "SHAPED_SCENE_LINEAR")
        << '\n';

    output
        << "# ORBIT_SHAPER "
        << (lut.metadata.shaper ==
                    ColorLutShaper::None
                ? "NONE"
                : "LOG2")
        << '\n';

    if (!lut.metadata.title.empty())
    {
        output
            << "TITLE \""
            << lut.metadata.title
            << "\"\n";
    }

    output
        << "LUT_3D_SIZE "
        << lut.size
        << '\n';

    output
        << "DOMAIN_MIN "
        << lut.metadata.domainMinimum
        << ' '
        << lut.metadata.domainMinimum
        << ' '
        << lut.metadata.domainMinimum
        << '\n';

    output
        << "DOMAIN_MAX "
        << lut.metadata.domainMaximum
        << ' '
        << lut.metadata.domainMaximum
        << ' '
        << lut.metadata.domainMaximum
        << '\n';

    for (u32 blue = 0U;
         blue < lut.size;
         ++blue)
    {
        for (u32 green = 0U;
             green < lut.size;
             ++green)
        {
            for (u32 red = 0U;
                 red < lut.size;
                 ++red)
            {
                const u32 packedX =
                    blue * lut.size +
                    red;
                const u32 packedY =
                    green;
                const std::size_t offset =
                    (static_cast<std::size_t>(packedY) *
                         lut.size *
                         lut.size +
                     packedX) *
                    4U;

                output
                    << static_cast<f32>(
                           lut.rgba8[offset]) /
                           255.0F
                    << ' '
                    << static_cast<f32>(
                           lut.rgba8[offset + 1U]) /
                           255.0F
                    << ' '
                    << static_cast<f32>(
                           lut.rgba8[offset + 2U]) /
                           255.0F
                    << '\n';
            }
        }
    }

    return output.str();
}

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
    result.metadata = {
        .domain =
            ColorLutDomain::DisplayLinear,
        .shaper =
            ColorLutShaper::None,
        .domainMinimum = 0.0F,
        .domainMaximum = 1.0F,
        .title = "Orbit Identity",
        .explicitOrbitMetadata = true
    };
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
    : size_(data.size),
      domainMinimum_(
          data.metadata.domainMinimum),
      domainMaximum_(
          data.metadata.domainMaximum)
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

    if (!IsDisplayLutCompatible(data))
    {
        throw std::invalid_argument(
            "Display LUT requires DisplayLinear domain with no shaper.");
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

f32 GpuColorLut::DomainMinimum() const noexcept
{
    return domainMinimum_;
}

f32 GpuColorLut::DomainMaximum() const noexcept
{
    return domainMaximum_;
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
            .pushConstantDwords = 8U,
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
            .depthWrite = false,
            .colorAttachmentFormats = {
                rhi::TextureFormat::RGBA16_Float
            },
            .colorAttachmentCount = 1U
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

    const std::array<u32, 8> constants{
        bits(static_cast<f32>(lut.Size())),
        bits(std::clamp(
            settings.strength,
            0.0F,
            1.0F)),
        bits(settings.enabled ? 1.0F : 0.0F),
        bits(lut.DomainMinimum()),
        bits(lut.DomainMaximum()),
        0U,
        0U,
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
