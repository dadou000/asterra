#include <orbit/studio_ui/ReportThumbnailCache.hpp>

#include <orbit/render_view/Capture.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Fence.hpp>
#include <orbit/rhi/Resource.hpp>

#include <algorithm>
#include <cstring>
#include <exception>
#include <system_error>
#include <utility>

namespace orbit::studio_ui
{
namespace
{
constexpr std::size_t kMaxEntries = 24U;
constexpr u32 kRetireFrames = 8U;

// Box-filter shrink by an integer factor, so thumbnails stay cheap and clean.
[[nodiscard]] render_view::CapturedImage Shrink(
    const render_view::CapturedImage& image,
    const u32 factor)
{
    if (factor <= 1U)
    {
        return image;
    }
    render_view::CapturedImage out;
    out.width = std::max(1U, image.width / factor);
    out.height = std::max(1U, image.height / factor);
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4U);
    for (u32 y = 0U; y < out.height; ++y)
    {
        for (u32 x = 0U; x < out.width; ++x)
        {
            u32 sum[4] = {0U, 0U, 0U, 0U};
            u32 count = 0U;
            for (u32 dy = 0U; dy < factor; ++dy)
            {
                for (u32 dx = 0U; dx < factor; ++dx)
                {
                    const u32 sx = x * factor + dx;
                    const u32 sy = y * factor + dy;
                    if (sx >= image.width || sy >= image.height)
                    {
                        continue;
                    }
                    const u8* p =
                        image.rgba.data() + (static_cast<std::size_t>(sy) * image.width + sx) * 4U;
                    for (int c = 0; c < 4; ++c)
                    {
                        sum[c] += p[c];
                    }
                    ++count;
                }
            }
            u8* o = out.rgba.data() + (static_cast<std::size_t>(y) * out.width + x) * 4U;
            for (int c = 0; c < 4; ++c)
            {
                o[c] = static_cast<u8>(count == 0U ? 0U : sum[c] / count);
            }
        }
    }
    return out;
}
} // namespace

ReportThumbnailCache::ReportThumbnailCache(
    rhi::Device& device,
    rhi::Queue& graphicsQueue) noexcept
    : device_(&device),
      queue_(&graphicsQueue)
{
}

ReportThumbnailCache::~ReportThumbnailCache() = default;

ReportThumbnailCache::Entry ReportThumbnailCache::Load(
    const std::filesystem::path& path)
{
    Entry entry;
    std::error_code error;
    entry.modified = std::filesystem::last_write_time(path, error);
    if (error)
    {
        entry.failed = true;
        return entry;
    }

    try
    {
        render_view::CapturedImage image = render_view::ReadImageRgba8(path);
        if (image.width == 0U || image.height == 0U)
        {
            entry.failed = true;
            return entry;
        }
        const u32 factor = std::max(1U, (image.width + kMaxWidth - 1U) / kMaxWidth);
        image = Shrink(image, factor);

        entry.width = image.width;
        entry.height = image.height;
        entry.texture = device_->CreateTexture({
            .width = image.width,
            .height = image.height,
            .format = rhi::TextureFormat::RGBA8_UNorm,
            .initialState = rhi::ResourceState::Common});

        auto staging = device_->CreateBuffer({
            .sizeBytes = image.rgba.size(),
            .usage = rhi::BufferUsage::Generic,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::Common});
        std::memcpy(staging->Map(), image.rgba.data(), image.rgba.size());
        staging->Unmap();

        auto allocator = device_->CreateCommandAllocator(queue_->Type());
        auto commands = device_->CreateCommandList(*allocator);
        auto fence = device_->CreateFence(0);
        commands->Transition(
            *entry.texture,
            rhi::ResourceState::Common,
            rhi::ResourceState::CopyDestination);
        commands->CopyBufferToTexture(*staging, 0, *entry.texture);
        commands->Transition(
            *entry.texture,
            rhi::ResourceState::CopyDestination,
            rhi::ResourceState::ShaderResource);
        commands->Close();
        queue_->Submit(*commands);
        queue_->Signal(*fence, 1);
        fence->Wait(1);
    }
    catch (const std::exception&)
    {
        entry.texture.reset();
        entry.failed = true;
    }
    return entry;
}

ReportThumbnailCache::Thumbnail ReportThumbnailCache::Get(
    const std::filesystem::path& path)
{
    if (path.empty())
    {
        return {};
    }

    const std::string key = path.generic_string();
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(path, error);

    if (const auto found = entries_.find(key); found != entries_.end())
    {
        // A missing file stays a failure; a changed one is reloaded.
        if (error || found->second.modified == modified)
        {
            return {found->second.texture.get(), found->second.width, found->second.height};
        }
        if (found->second.texture != nullptr)
        {
            retired_.push_back({std::move(found->second.texture), 0U});
        }
        entries_.erase(found);
    }
    if (error)
    {
        return {};
    }

    if (entries_.size() >= kMaxEntries)
    {
        // Drop the oldest-inserted entry; it may still be on screen, so retire it.
        auto victim = entries_.begin();
        if (victim->second.texture != nullptr)
        {
            retired_.push_back({std::move(victim->second.texture), 0U});
        }
        entries_.erase(victim);
    }

    Entry loaded = Load(path);
    Thumbnail result{loaded.texture.get(), loaded.width, loaded.height};
    entries_.emplace(key, std::move(loaded));
    return result;
}

void ReportThumbnailCache::Tick()
{
    for (Retired& retired : retired_)
    {
        ++retired.age;
    }
    std::erase_if(
        retired_,
        [](const Retired& retired)
        {
            return retired.age > kRetireFrames;
        });
}
} // namespace orbit::studio_ui
