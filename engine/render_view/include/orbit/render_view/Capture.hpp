#pragma once

#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Queue.hpp>

#include <filesystem>
#include <optional>
#include <string_view>

namespace orbit::render_view
{
struct CaptureResult
{
    std::filesystem::path path;
    u32 width{0};
    u32 height{0};
    u64 fileBytes{0};
};

// Synchronously captures the completed RGBA8 display target of a RenderView:
// the exposed, tone-mapped and graded image Studio presents. The caller must
// ensure rendering to the view has completed before calling; Studio does this
// immediately after waiting its previous-frame fence.
[[nodiscard]] CaptureResult CaptureBmp(
    rhi::Device& device,
    rhi::Queue& graphicsQueue,
    RenderView& view,
    const std::filesystem::path& path);

// Physical render targets that can be captured losslessly for diagnostics.
enum class CaptureBuffer
{
    SceneColor,
    SurfaceBaseRoughness,
    SurfaceNormalMetallic,
    SurfaceEmissionClass,
    DisplayLinear
};

[[nodiscard]] std::optional<CaptureBuffer>
ParseCaptureBuffer(
    std::string_view name) noexcept;

// Writes a float render target as an "OFB1" file: 4-byte magic, u32 width,
// u32 height, u32 channel count (4), then top-down row-major float32 RGBA.
// Values are the exact scene-linear/G-buffer contents, never display encoded.
[[nodiscard]] CaptureResult CaptureFloatBuffer(
    rhi::Device& device,
    rhi::Queue& graphicsQueue,
    RenderView& view,
    CaptureBuffer buffer,
    const std::filesystem::path& path);
} // namespace orbit::render_view
