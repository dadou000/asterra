#pragma once

#include <orbit/content/RuntimeTexture.hpp>

#include <cstddef>
#include <filesystem>
#include <span>

namespace orbit::content
{
class ImporterRegistry;
}

namespace orbit::content_wic
{
// Registers Windows Imaging Component source decoders that produce Orbit's
// platform-neutral runtime texture container.
void RegisterTextureImporters(
    content::ImporterRegistry& registry);

// Decodes a single image file straight to RGBA8 pixels, bypassing the
// ImporterRegistry/derived-data-cache pipeline. For a live-preview consumer
// (the Shading tab's texture2d parameters) that wants today's file contents
// right now, not a cached cooked artifact -- the same PNG/JPG/TGA/etc.
// formats RegisterTextureImporters registers, decoded through the same WIC
// path. Throws std::runtime_error on a missing or unreadable file.
[[nodiscard]] content::RuntimeTexture DecodeTextureFile(
    const std::filesystem::path& path);

// Decodes an encoded image held in memory (PNG, JPEG, BMP, ...) to RGBA8,
// e.g. a texture embedded in a GLB. Throws std::runtime_error when the
// buffer is empty or not a decodable image.
[[nodiscard]] content::RuntimeTexture DecodeTextureMemory(
    std::span<const std::byte> encoded);
} // namespace orbit::content_wic
