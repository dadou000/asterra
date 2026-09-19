#pragma once

#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Resource.hpp>
#include <orbit/terrain_debug/TerrainDebugRaster.hpp>

#include <memory>

namespace orbit::terrain_debug
{
// Persistent derived GPU image for one physical-page debug raster. The object
// never owns terrain authority; each Upload() composes from a typed M29 source
// binding and stages that derived RGBA8 image into an RHI texture.
class TerrainDebugTexture
{
public:
    TerrainDebugTexture(
        rhi::Device& device,
        u32 width,
        u32 height);

    TerrainDebugTexture(const TerrainDebugTexture&) = delete;
    TerrainDebugTexture& operator=(const TerrainDebugTexture&) = delete;

    void Upload(
        rhi::CommandList& commands,
        const TerrainDebugRasterView& view);

    [[nodiscard]] u32 Width() const noexcept;
    [[nodiscard]] u32 Height() const noexcept;
    [[nodiscard]] bool HasContent() const noexcept;

    [[nodiscard]] rhi::Texture& Texture() noexcept;
    [[nodiscard]] const rhi::Texture& Texture() const noexcept;

private:
    u32 width_{0};
    u32 height_{0};
    bool uploaded_{false};

    std::unique_ptr<rhi::Buffer> staging_;
    std::unique_ptr<rhi::Texture> texture_;
};
} // namespace orbit::terrain_debug
