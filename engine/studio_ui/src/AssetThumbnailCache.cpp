#include <orbit/studio_ui/AssetThumbnailCache.hpp>

#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Resource.hpp>

#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] u16 Read16(
    const std::span<const std::byte> bytes,
    const std::size_t offset)
{
    if (offset + 2U > bytes.size())
    {
        throw std::runtime_error("Truncated BMP header.");
    }

    return static_cast<u16>(
        std::to_integer<u8>(bytes[offset]) |
        (static_cast<u16>(std::to_integer<u8>(bytes[offset + 1U])) << 8U));
}

[[nodiscard]] u32 Read32(
    const std::span<const std::byte> bytes,
    const std::size_t offset)
{
    if (offset + 4U > bytes.size())
    {
        throw std::runtime_error("Truncated BMP header.");
    }

    u32 value = 0U;
    for (u32 byte = 0U; byte < 4U; ++byte)
    {
        value |= static_cast<u32>(
                     std::to_integer<u8>(bytes[offset + byte]))
            << (byte * 8U);
    }
    return value;
}

[[nodiscard]] std::vector<std::byte> ReadFile(
    const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        throw std::runtime_error("Cannot open thumbnail BMP.");
    }

    const std::streamsize size = stream.tellg();
    if (size <= 0)
    {
        throw std::runtime_error("Thumbnail BMP is empty.");
    }

    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!stream.read(
            reinterpret_cast<char*>(bytes.data()),
            size))
    {
        throw std::runtime_error("Cannot read thumbnail BMP.");
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> DecodeThumbnailBmp(
    const std::filesystem::path& path,
    const u32 expectedWidth,
    const u32 expectedHeight)
{
    const std::vector<std::byte> storage = ReadFile(path);
    const std::span<const std::byte> bytes(storage);

    if (bytes.size() < 54U ||
        std::to_integer<u8>(bytes[0]) != static_cast<u8>('B') ||
        std::to_integer<u8>(bytes[1]) != static_cast<u8>('M'))
    {
        throw std::runtime_error("Thumbnail cache entry is not a BMP file.");
    }

    const u32 pixelOffset = Read32(bytes, 10U);
    const u32 dibBytes = Read32(bytes, 14U);
    const u32 width = Read32(bytes, 18U);
    const u32 height = Read32(bytes, 22U);
    const u16 planes = Read16(bytes, 26U);
    const u16 bitsPerPixel = Read16(bytes, 28U);
    const u32 compression = Read32(bytes, 30U);

    if (dibBytes < 40U || planes != 1U || bitsPerPixel != 32U ||
        compression != 0U || width != expectedWidth ||
        height != expectedHeight)
    {
        throw std::runtime_error(
            "Thumbnail BMP does not match Orbit's cached RGBA8 format.");
    }

    const u64 pixelBytes =
        static_cast<u64>(width) * static_cast<u64>(height) * 4ULL;
    if (pixelBytes >
            static_cast<u64>(std::numeric_limits<std::size_t>::max()) ||
        static_cast<u64>(pixelOffset) + pixelBytes > bytes.size())
    {
        throw std::runtime_error("Thumbnail BMP pixel payload is truncated.");
    }

    std::vector<std::byte> rgba(static_cast<std::size_t>(pixelBytes));
    for (u32 y = 0U; y < height; ++y)
    {
        const u32 sourceY = height - 1U - y;
        for (u32 x = 0U; x < width; ++x)
        {
            const std::size_t source =
                static_cast<std::size_t>(pixelOffset) +
                (static_cast<std::size_t>(sourceY) * width + x) * 4U;
            const std::size_t destination =
                (static_cast<std::size_t>(y) * width + x) * 4U;

            rgba[destination + 0U] = bytes[source + 2U];
            rgba[destination + 1U] = bytes[source + 1U];
            rgba[destination + 2U] = bytes[source + 0U];
            rgba[destination + 3U] = bytes[source + 3U];
        }
    }
    return rgba;
}
} // namespace

AssetThumbnailCache::AssetThumbnailCache(
    rhi::Device& device,
    rhi::Queue& graphicsQueue,
    content::ContentService& content)
    : device_(device),
      graphicsQueue_(graphicsQueue),
      content_(content),
      allocator_(device.CreateCommandAllocator(rhi::QueueType::Graphics)),
      commands_(device.CreateCommandList(*allocator_)),
      fence_(device.CreateFence(0U))
{
}

AssetThumbnailCache::~AssetThumbnailCache() = default;

rhi::Texture* AssetThumbnailCache::Get(
    const std::filesystem::path& assetPath) noexcept
{
    try
    {
        const content::AssetRecord* asset =
            content_.FindByPath(assetPath);
        if (asset == nullptr)
        {
            return nullptr;
        }

        const content::ThumbnailResult thumbnail =
            content_.GetThumbnail(asset->id, kSize, kSize);
        const std::string key = asset->sourcePath.generic_string();
        Entry& entry = entries_[key];

        if (entry.key == thumbnail.key)
        {
            return entry.failed ? nullptr : entry.texture.get();
        }

        entry.key = thumbnail.key;
        entry.texture.reset();
        entry.failed = false;

        try
        {
            entry.texture = UploadBmp(
                thumbnail.path,
                thumbnail.width,
                thumbnail.height);
        }
        catch (...)
        {
            entry.failed = true;
            return nullptr;
        }

        return entry.texture.get();
    }
    catch (...)
    {
        return nullptr;
    }
}

std::unique_ptr<rhi::Texture>
AssetThumbnailCache::UploadBmp(
    const std::filesystem::path& path,
    const u32 expectedWidth,
    const u32 expectedHeight)
{
    const std::vector<std::byte> rgba =
        DecodeThumbnailBmp(path, expectedWidth, expectedHeight);

    auto texture = device_.CreateTexture({
        .width = expectedWidth,
        .height = expectedHeight,
        .format = rhi::TextureFormat::RGBA8_UNorm,
        .initialState = rhi::ResourceState::Common
    });

    auto staging = device_.CreateBuffer({
        .sizeBytes = rgba.size(),
        .usage = rhi::BufferUsage::Generic,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::Common
    });
    std::memcpy(staging->Map(), rgba.data(), rgba.size());
    staging->Unmap();

    allocator_->Reset();
    commands_->Reset(*allocator_);
    commands_->Transition(
        *texture,
        rhi::ResourceState::Common,
        rhi::ResourceState::CopyDestination);
    commands_->CopyBufferToTexture(*staging, 0U, *texture);
    commands_->Transition(
        *texture,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::ShaderResource);
    commands_->Close();

    graphicsQueue_.Submit(*commands_);
    graphicsQueue_.Signal(*fence_, ++fenceValue_);
    fence_->Wait(fenceValue_);

    return texture;
}
} // namespace orbit::studio_ui
