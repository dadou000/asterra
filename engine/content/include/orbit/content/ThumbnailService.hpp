#pragma once

#include <orbit/content/ContentHash.hpp>
#include <orbit/content/DerivedDataCache.hpp>
#include <orbit/core/Types.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::content
{
struct ThumbnailRequest
{
    ContentHash sourceHash;
    std::string category;
    std::string label;
    u32 width{96};
    u32 height{96};
};

struct ThumbnailPixels
{
    u32 width{0};
    u32 height{0};
    std::vector<std::byte> rgba8;
};

struct ThumbnailProviderDescriptor
{
    std::string id;
    u32 version{1};
    std::string category;
    std::function<ThumbnailPixels(
        const ThumbnailRequest&)> generate;
};

struct ThumbnailResult
{
    ContentHash key;
    std::filesystem::path path;
    u32 width{0};
    u32 height{0};
    bool cacheHit{false};
    std::string providerId;
};

class ThumbnailService
{
public:
    explicit ThumbnailService(
        DerivedDataCache& cache);

    void RegisterProvider(
        ThumbnailProviderDescriptor provider);

    // Generates or retrieves a cached BMP thumbnail. Categories without a
    // specialized provider use Orbit's permanent deterministic fallback icon,
    // so every indexed asset always has a visual identity.
    [[nodiscard]] ThumbnailResult Get(
        const ThumbnailRequest& request);

private:
    [[nodiscard]] const ThumbnailProviderDescriptor*
    FindProvider(
        std::string_view category) const;

    DerivedDataCache& cache_;
    std::vector<ThumbnailProviderDescriptor>
        providers_;
};
} // namespace orbit::content
