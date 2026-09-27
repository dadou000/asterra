#pragma once

#include <orbit/content/RuntimeTexture.hpp>

#include <filesystem>

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
} // namespace orbit::content_wic
