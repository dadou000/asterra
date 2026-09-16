#include <orbit/render_view/Capture.hpp>

#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Fence.hpp>
#include <orbit/rhi/Resource.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
} // namespace

CaptureResult CaptureBmp(
    rhi::Device& device,
    rhi::Queue& graphicsQueue,
    RenderView& view,
    const std::filesystem::path& path)
{
    if (view.Color().Format() !=
        rhi::TextureFormat::RGBA8_UNorm)
    {
        throw std::invalid_argument(
            "RenderView BMP capture requires an RGBA8 color target.");
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
        view.Color(),
        rhi::ResourceState::ShaderResource,
        rhi::ResourceState::CopySource);

    commands->CopyTextureToBuffer(
        view.Color(),
        *readback);

    commands->Transition(
        view.Color(),
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
                static_cast<char>(pixel[2]),
                static_cast<char>(pixel[1]),
                static_cast<char>(pixel[0]),
                static_cast<char>(pixel[3])
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
