#pragma once

#include <orbit/content/ContentService.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Queue.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/shading/ShaderPreviewRenderer.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace orbit::shading
{
// Small, static sphere-preview thumbnails for the Shading tree's shader and
// shader material rows, so browsing looks like Blender/Unreal's material
// list rather than a bare file tree.
//
// Get() is cheap to call every frame for every visible row: a fresh entry (or
// one whose material or resolved shader's ContentHash changed -- editing the
// shader invalidates every material built on it too) renders synchronously
// on the shared graphics queue, the same one-shot pattern
// ShaderPreviewRenderer::UploadTexture already uses; an unchanged entry is a
// map lookup and two hash comparisons. A shader that fails to compile still
// renders (the shared error checker), so a broken material reads as broken
// rather than disappearing from the tree.
class MaterialThumbnailCache
{
public:
    static constexpr u32 kSize = 96U;

    MaterialThumbnailCache(
        rhi::Device& device,
        const shader::Compiler& compiler,
        rhi::Queue& graphicsQueue,
        content::ContentService& content);
    ~MaterialThumbnailCache();

    MaterialThumbnailCache(const MaterialThumbnailCache&) = delete;
    MaterialThumbnailCache& operator=(const MaterialThumbnailCache&) = delete;

    // Null only for a path that isn't a previewable shader/material asset at
    // all (a folder, a mesh, something unreadable).
    [[nodiscard]] rhi::Texture* Get(const std::filesystem::path& path);

private:
    struct Entry
    {
        content::ContentHash materialHash{};
        content::ContentHash shaderHash{};
        std::unique_ptr<rhi::Texture> color;
        bool everRendered{false};
    };

    [[nodiscard]] bool IsFresh(
        const Entry& entry, const std::filesystem::path& path) const;
    void Render(Entry& entry, const std::filesystem::path& path);

    content::ContentService& content_;
    rhi::Device& device_;
    const shader::Compiler& compiler_;
    rhi::Queue& graphicsQueue_;
    ShaderPreviewRenderer renderer_;
    std::unique_ptr<rhi::Texture> depth_;
    std::unique_ptr<rhi::CommandAllocator> allocator_;
    std::unique_ptr<rhi::CommandList> commands_;
    std::unique_ptr<rhi::Fence> fence_;
    u64 fenceValue_{0U};
    u64 programRevision_{0U};
    std::map<std::string, Entry> entries_;
};
} // namespace orbit::shading
