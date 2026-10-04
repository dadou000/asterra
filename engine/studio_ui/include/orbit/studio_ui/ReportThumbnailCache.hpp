#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Queue.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace orbit::studio_ui
{
// Loads report screenshots (PNG) into small sampled textures so the Reports panel
// can show the picture of a problem without anyone launching a test. Entries are
// keyed by path and modification time, so a retaken screenshot is picked up.
// Images are shrunk to kMaxWidth. A texture dropped from the cache is kept alive
// for a few frames (Tick) because earlier frames may still be drawing it.
class ReportThumbnailCache
{
public:
    inline static constexpr u32 kMaxWidth = 640U;

    ReportThumbnailCache(rhi::Device& device, rhi::Queue& graphicsQueue) noexcept;
    ~ReportThumbnailCache();

    ReportThumbnailCache(const ReportThumbnailCache&) = delete;
    ReportThumbnailCache& operator=(const ReportThumbnailCache&) = delete;

    struct Thumbnail
    {
        rhi::Texture* texture{nullptr};
        u32 width{0U};
        u32 height{0U};
    };

    // Null texture when the file is missing or cannot be read (the error is
    // remembered per file, so a bad file is not retried every frame).
    [[nodiscard]] Thumbnail Get(const std::filesystem::path& path);

    // Once per frame.
    void Tick();

private:
    struct Entry
    {
        std::unique_ptr<rhi::Texture> texture;
        u32 width{0U};
        u32 height{0U};
        std::filesystem::file_time_type modified{};
        bool failed{false};
    };

    struct Retired
    {
        std::unique_ptr<rhi::Texture> texture;
        u32 age{0U};
    };

    [[nodiscard]] Entry Load(const std::filesystem::path& path);

    rhi::Device* device_{nullptr};
    rhi::Queue* queue_{nullptr};
    std::map<std::string, Entry> entries_;
    std::vector<Retired> retired_;
};
} // namespace orbit::studio_ui
