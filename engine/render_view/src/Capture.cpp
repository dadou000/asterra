#include <orbit/render_view/Capture.hpp>

#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Fence.hpp>
#include <orbit/rhi/Resource.hpp>

#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace orbit::render_view
{
namespace
{
void WriteU16(
    std::ostream& output,
    const u16 value)
{
    const std::array<char, 2> bytes{
        static_cast<char>(value & 0xFFU),
        static_cast<char>((value >> 8U) & 0xFFU)
    };

    output.write(
        bytes.data(),
        static_cast<std::streamsize>(
            bytes.size()));
}

void WriteU32(
    std::ostream& output,
    const u32 value)
{
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xFFU),
        static_cast<char>((value >> 8U) & 0xFFU),
        static_cast<char>((value >> 16U) & 0xFFU),
        static_cast<char>((value >> 24U) & 0xFFU)
    };

    output.write(
        bytes.data(),
        static_cast<std::streamsize>(
            bytes.size()));
}

void WriteI32(
    std::ostream& output,
    const i32 value)
{
    WriteU32(
        output,
        static_cast<u32>(value));
}

[[nodiscard]] std::vector<std::byte> ReadbackTexture(
    rhi::Device& device,
    rhi::Queue& graphicsQueue,
    rhi::Texture& texture,
    const u64 bytesPerPixel)
{
    const u64 pixelBytes =
        static_cast<u64>(texture.Width()) *
        static_cast<u64>(texture.Height()) *
        bytesPerPixel;

    auto readback =
        device.CreateBuffer({
            .sizeBytes = pixelBytes,
            .usage =
                rhi::BufferUsage::Generic,
            .memory =
                rhi::MemoryUsage::HostReadback,
            .initialState =
                rhi::ResourceState::
                    CopyDestination
        });

    auto allocator =
        device.CreateCommandAllocator(
            graphicsQueue.Type());

    auto commands =
        device.CreateCommandList(
            *allocator);

    auto fence =
        device.CreateFence(0);

    commands->Transition(
        texture,
        rhi::ResourceState::ShaderResource,
        rhi::ResourceState::CopySource);

    commands->CopyTextureToBuffer(
        texture,
        *readback);

    commands->Transition(
        texture,
        rhi::ResourceState::CopySource,
        rhi::ResourceState::ShaderResource);

    commands->Close();

    graphicsQueue.Submit(
        *commands);
    graphicsQueue.Signal(
        *fence,
        1);
    fence->Wait(1);

    std::vector<std::byte> bytes(
        static_cast<std::size_t>(
            pixelBytes));

    const std::byte* mapped =
        readback->Map();
    std::memcpy(
        bytes.data(),
        mapped,
        bytes.size());
    readback->Unmap();

    return bytes;
}

[[nodiscard]] std::filesystem::path PrepareCapturePath(
    const std::filesystem::path& path)
{
    if (path.empty())
    {
        throw std::invalid_argument(
            "RenderView capture path must not be empty.");
    }

    std::filesystem::path absolutePath =
        path.is_relative()
            ? std::filesystem::absolute(path)
            : path;

    if (const auto parent =
            absolutePath.parent_path();
        !parent.empty())
    {
        std::filesystem::create_directories(
            parent);
    }

    return absolutePath;
}

[[nodiscard]] f32 HalfToFloat(
    const u16 half) noexcept
{
    const u32 sign =
        static_cast<u32>(half & 0x8000U) << 16U;
    const u32 exponent =
        (half >> 10U) & 0x1FU;
    const u32 mantissa =
        half & 0x03FFU;

    if (exponent == 0U)
    {
        // Zero or subnormal: value = mantissa * 2^-24.
        const f32 magnitude =
            static_cast<f32>(mantissa) *
            5.9604644775390625e-8F;
        return sign != 0U
            ? -magnitude
            : magnitude;
    }

    const u32 bits =
        exponent == 0x1FU
            ? sign | 0x7F800000U | (mantissa << 13U)
            : sign |
                  ((exponent + 112U) << 23U) |
                  (mantissa << 13U);

    return std::bit_cast<f32>(bits);
}
} // namespace

std::optional<CaptureBuffer> ParseCaptureBuffer(
    const std::string_view name) noexcept
{
    if (name == "color" || name == "scene_color")
    {
        return CaptureBuffer::SceneColor;
    }

    if (name == "base_roughness")
    {
        return CaptureBuffer::SurfaceBaseRoughness;
    }

    if (name == "normal_metallic")
    {
        return CaptureBuffer::SurfaceNormalMetallic;
    }

    if (name == "emission_class")
    {
        return CaptureBuffer::SurfaceEmissionClass;
    }

    if (name == "display_linear")
    {
        return CaptureBuffer::DisplayLinear;
    }

    return std::nullopt;
}

CaptureResult CaptureFloatBuffer(
    rhi::Device& device,
    rhi::Queue& graphicsQueue,
    RenderView& view,
    const CaptureBuffer buffer,
    const std::filesystem::path& path)
{
    rhi::Texture* texture = nullptr;

    switch (buffer)
    {
    case CaptureBuffer::SceneColor:
        texture = &view.Color();
        break;
    case CaptureBuffer::SurfaceBaseRoughness:
        texture = &view.SurfaceBaseRoughness();
        break;
    case CaptureBuffer::SurfaceNormalMetallic:
        texture = &view.SurfaceNormalMetallic();
        break;
    case CaptureBuffer::SurfaceEmissionClass:
        texture = &view.SurfaceEmissionClass();
        break;
    case CaptureBuffer::DisplayLinear:
        texture = &view.DisplayLinear();
        break;
    }

    if (texture == nullptr ||
        texture->Format() !=
            rhi::TextureFormat::RGBA16_Float)
    {
        throw std::invalid_argument(
            "RenderView float capture requires an RGBA16F target.");
    }

    const u32 width = texture->Width();
    const u32 height = texture->Height();

    const auto bytes =
        ReadbackTexture(
            device,
            graphicsQueue,
            *texture,
            8U);

    const auto absolutePath =
        PrepareCapturePath(path);

    std::ofstream output(
        absolutePath,
        std::ios::binary |
        std::ios::trunc);

    if (!output)
    {
        throw std::runtime_error(
            "Orbit could not open the viewport buffer capture destination.");
    }

    output.write("OFB1", 4);
    WriteU32(output, width);
    WriteU32(output, height);
    WriteU32(output, 4U);

    const std::size_t count =
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height) *
        4U;

    std::vector<f32> values(count);

    for (std::size_t index = 0;
         index < count;
         ++index)
    {
        u16 half = 0U;
        std::memcpy(
            &half,
            bytes.data() + index * 2U,
            sizeof(half));
        values[index] = HalfToFloat(half);
    }

    output.write(
        reinterpret_cast<const char*>(values.data()),
        static_cast<std::streamsize>(
            values.size() * sizeof(f32)));

    if (!output)
    {
        throw std::runtime_error(
            "Orbit failed while writing the viewport buffer capture.");
    }

    output.close();

    return {
        .path = absolutePath,
        .width = width,
        .height = height,
        .fileBytes =
            16U + static_cast<u64>(values.size()) * sizeof(f32)
    };
}

CaptureResult CaptureBmp(
    rhi::Device& device,
    rhi::Queue& graphicsQueue,
    RenderView& view,
    const std::filesystem::path& path)
{
    rhi::Texture& display =
        view.DisplayColor();

    if (display.Format() !=
        rhi::TextureFormat::RGBA8_UNorm)
    {
        throw std::invalid_argument(
            "RenderView BMP capture requires an RGBA8 display target.");
    }

    const u32 width =
        view.Width();
    const u32 height =
        view.Height();

    const u64 pixelBytes =
        static_cast<u64>(width) *
        static_cast<u64>(height) *
        4U;

    auto readback =
        device.CreateBuffer({
            .sizeBytes = pixelBytes,
            .usage =
                rhi::BufferUsage::Generic,
            .memory =
                rhi::MemoryUsage::HostReadback,
            .initialState =
                rhi::ResourceState::
                    CopyDestination
        });

    auto allocator =
        device.CreateCommandAllocator(
            graphicsQueue.Type());

    auto commands =
        device.CreateCommandList(
            *allocator);

    auto fence =
        device.CreateFence(0);

    commands->Transition(
        display,
        rhi::ResourceState::ShaderResource,
        rhi::ResourceState::CopySource);

    commands->CopyTextureToBuffer(
        display,
        *readback);

    commands->Transition(
        display,
        rhi::ResourceState::CopySource,
        rhi::ResourceState::ShaderResource);

    commands->Close();

    graphicsQueue.Submit(
        *commands);
    graphicsQueue.Signal(
        *fence,
        1);
    fence->Wait(1);

    std::filesystem::path absolutePath =
        path;

    if (absolutePath.empty())
    {
        throw std::invalid_argument(
            "RenderView capture path must not be empty.");
    }

    if (absolutePath.is_relative())
    {
        absolutePath =
            std::filesystem::absolute(
                absolutePath);
    }

    if (const auto parent =
            absolutePath.parent_path();
        !parent.empty())
    {
        std::filesystem::create_directories(
            parent);
    }

    std::ofstream output(
        absolutePath,
        std::ios::binary |
        std::ios::trunc);

    if (!output)
    {
        throw std::runtime_error(
            "Orbit could not open the viewport screenshot destination.");
    }

    constexpr u32 fileHeaderBytes = 14;
    constexpr u32 infoHeaderBytes = 40;

    if (pixelBytes >
        static_cast<u64>(
            std::numeric_limits<u32>::max() -
            fileHeaderBytes -
            infoHeaderBytes))
    {
        throw std::overflow_error(
            "Viewport screenshot is too large for BMP.");
    }

    const u32 fileBytes =
        fileHeaderBytes +
        infoHeaderBytes +
        static_cast<u32>(
            pixelBytes);

    // BITMAPFILEHEADER
    WriteU16(output, 0x4D42U);
    WriteU32(output, fileBytes);
    WriteU16(output, 0U);
    WriteU16(output, 0U);
    WriteU32(
        output,
        fileHeaderBytes +
            infoHeaderBytes);

    // BITMAPINFOHEADER. Positive height makes the file bottom-up.
    WriteU32(output, infoHeaderBytes);
    WriteI32(
        output,
        static_cast<i32>(width));
    WriteI32(
        output,
        static_cast<i32>(height));
    WriteU16(output, 1U);
    WriteU16(output, 32U);
    WriteU32(output, 0U);
    WriteU32(
        output,
        static_cast<u32>(
            pixelBytes));
    WriteI32(output, 0);
    WriteI32(output, 0);
    WriteU32(output, 0U);
    WriteU32(output, 0U);

    std::byte* mapped =
        readback->Map();

    // Vulkan RGBA8 -> BMP BGRA, with row reversal for BMP's bottom-up
    // storage. A 32-bit BMP row needs no extra padding.
    for (u32 outputRow = 0;
         outputRow < height;
         ++outputRow)
    {
        const u32 sourceRow =
            height - 1U - outputRow;

        const std::byte* source =
            mapped +
            static_cast<std::size_t>(
                sourceRow) *
                static_cast<std::size_t>(
                    width) *
                4U;

        for (u32 column = 0;
             column < width;
             ++column)
        {
            const std::byte* pixel =
                source +
                static_cast<std::size_t>(
                    column) *
                    4U;

            const std::array<char, 4> bgra{
                static_cast<char>(
                    std::to_integer<u8>(pixel[2])),
                static_cast<char>(
                    std::to_integer<u8>(pixel[1])),
                static_cast<char>(
                    std::to_integer<u8>(pixel[0])),
                static_cast<char>(
                    std::to_integer<u8>(pixel[3]))
            };

            output.write(
                bgra.data(),
                static_cast<std::streamsize>(
                    bgra.size()));
        }
    }

    readback->Unmap();

    if (!output)
    {
        throw std::runtime_error(
            "Orbit failed while writing the viewport screenshot.");
    }

    output.close();

    return {
        .path = std::move(absolutePath),
        .width = width,
        .height = height,
        .fileBytes = fileBytes
    };
}
} // namespace orbit::render_view
