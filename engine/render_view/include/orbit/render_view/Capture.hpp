#pragma once

#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Queue.hpp>

#include <filesystem>

namespace orbit::render_view
{
struct CaptureResult
{
    std::filesystem::path path;
    u32 width{0};
    u32 height{0};
    u64 fileBytes{0};
};

// Synchronously captures the completed RGBA8 color target of a RenderView.
// The caller must ensure rendering to the view has completed before calling;
// Studio does this immediately after waiting its previous-frame fence.
[[nodiscard]] CaptureResult CaptureBmp(
    rhi::Device& device,
    rhi::Queue& graphicsQueue,
    RenderView& view,
    const std::filesystem::path& path);
} // namespace orbit::render_view
