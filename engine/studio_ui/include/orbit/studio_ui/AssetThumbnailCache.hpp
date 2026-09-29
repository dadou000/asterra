#pragma once

#include <orbit/content/ContentService.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Queue.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace orbit::studio_ui
{
// GPU-side presentation cache for ContentService thumbnails. ContentService
// remains the single thumbnail authority; this class only decodes its cached
// 32-bit BMP artifact and uploads it once for Dear ImGui consumption.
class AssetThumbnailCache
{
public:
    static constexpr u32 kSize = 64U;

    AssetThumbnailCache(
        rhi::Device& device,
        rhi::Queue& graphicsQueue,
        content::ContentService& content);
    ~AssetThumbnailCache();

    AssetThumbnailCache(const AssetThumbnailCache&) = delete;
    AssetThumbnailCache& operator=(const AssetThumbnailCache&) = delete;

    // Null for an unknown asset or when thumbnail generation/upload fails.
    // Failed keys are remembered, so a broken thumbnail is not retried every
    // frame; a changed source hash produces a new key and retries naturally.
    [[nodiscard]] rhi::Texture* Get(
        const std::filesystem::path& assetPath) noexcept;

private:
    struct Entry
    {
        content::ContentHash key{};
        std::unique_ptr<rhi::Texture> texture;
        bool failed{false};
    };

    [[nodiscard]] std::unique_ptr<rhi::Texture>
    UploadBmp(
        const std::filesystem::path& path,
        u32 expectedWidth,
        u32 expectedHeight);

    rhi::Device& device_;
    rhi::Queue& graphicsQueue_;
    content::ContentService& content_;
    std::unique_ptr<rhi::CommandAllocator> allocator_;
    std::unique_ptr<rhi::CommandList> commands_;
    std::unique_ptr<rhi::Fence> fence_;
    u64 fenceValue_{0U};
    std::map<std::string, Entry> entries_;
};
} // namespace orbit::studio_ui
