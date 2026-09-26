#pragma once

#include <orbit/core/Types.hpp>

#include <filesystem>
#include <vector>

// GPU-free helpers for turning the preview's float render target into a
// screenshot. The device readback lives with RenderView capture; these only
// convert its "OFB1" dump to a BMP so `shading.screenshot` yields an image any
// tool can open.
namespace orbit::shading
{
struct FloatImage
{
    u32 width{0U};
    u32 height{0U};
    // Top-down, row-major RGBA, four floats per pixel.
    std::vector<f32> rgba;
};

// Reads an "OFB1" file: 4-byte magic, u32 width, u32 height, u32 channels (4),
// then float32 RGBA. Throws on a malformed or truncated file.
[[nodiscard]] FloatImage ReadFloatCapture(const std::filesystem::path& path);

// Writes a top-down 32-bit BMP, clamping each channel to [0, 1]. The shading
// wrapper already display-encodes its output, so no further transfer function
// is applied.
void WriteBmp32(const std::filesystem::path& path, const FloatImage& image);
} // namespace orbit::shading
