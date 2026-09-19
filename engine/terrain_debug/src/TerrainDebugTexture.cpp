#include <orbit/terrain_debug/TerrainDebugTexture.hpp>

#include <cstring>
#include <stdexcept>

namespace orbit::terrain_debug
{
TerrainDebugTexture::TerrainDebugTexture(
    rhi::Device& device,
    const u32 width,
    const u32 height)
    : width_(width),
      height_(height)
{
    if (width_ == 0U || height_ == 0U)
    {
        throw std::invalid_argument(
            "Terrain debug texture dimensions must be non-zero.");
    }

    const u64 texels =
        static_cast<u64>(width_) *
        static_cast<u64>(height_);
    const u64 bytes =
        texels *
        static_cast<u64>(
            rhi::TextureFormatBytesPerTexel(
                rhi::TextureFormat::RGBA8_UNorm));

    staging_ =
        device.CreateBuffer({
            .sizeBytes = bytes,
            .usage = rhi::BufferUsage::Generic,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::CopySource
        });

    texture_ =
        device.CreateTexture({
            .width = width_,
            .height = height_,
            .format = rhi::TextureFormat::RGBA8_UNorm,
            .initialState = rhi::ResourceState::CopyDestination
        });
}

void TerrainDebugTexture::Upload(
    rhi::CommandList& commands,
    const TerrainDebugRasterView& view)
{
    if (view.width != width_ ||
        view.height != height_)
    {
        throw std::invalid_argument(
            "Terrain debug raster dimensions do not match the GPU debug texture.");
    }

    const auto rgba =
        ComposeTerrainDebugRgba8(view);

    const u64 expectedBytes =
        static_cast<u64>(rgba.size());

    if (staging_->SizeBytes() != expectedBytes)
    {
        throw std::logic_error(
            "Terrain debug staging size does not match composed RGBA8 data.");
    }

    std::byte* mapped = staging_->Map();
    std::memcpy(
        mapped,
        rgba.data(),
        rgba.size());
    staging_->Unmap();

    if (uploaded_)
    {
        commands.Transition(
            *texture_,
            rhi::ResourceState::ShaderResource,
            rhi::ResourceState::CopyDestination);
    }

    commands.CopyBufferToTexture(
        *staging_,
        0,
        *texture_);

    commands.Transition(
        *texture_,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::ShaderResource);

    uploaded_ = true;
}

u32 TerrainDebugTexture::Width() const noexcept
{
    return width_;
}

u32 TerrainDebugTexture::Height() const noexcept
{
    return height_;
}

bool TerrainDebugTexture::HasContent() const noexcept
{
    return uploaded_;
}

rhi::Texture& TerrainDebugTexture::Texture() noexcept
{
    return *texture_;
}

const rhi::Texture& TerrainDebugTexture::Texture() const noexcept
{
    return *texture_;
}
} // namespace orbit::terrain_debug
