#pragma once

#include <orbit/core/Types.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace orbit::content
{
inline constexpr u32 kRuntimeTextureVersion = 1;

enum class RuntimeTextureFormat : u32
{
    Rgba8Unorm = 1
};

struct RuntimeTexture
{
    u32 width{0};
    u32 height{0};
    RuntimeTextureFormat format{
        RuntimeTextureFormat::Rgba8Unorm};
    std::vector<std::byte> pixels;
};

[[nodiscard]] std::vector<std::byte>
EncodeRuntimeTextureRgba8(
    u32 width,
    u32 height,
    std::span<const std::byte> pixels);

[[nodiscard]] RuntimeTexture
DecodeRuntimeTexture(
    std::span<const std::byte> bytes);
} // namespace orbit::content
