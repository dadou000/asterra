#include <orbit/content/ThumbnailService.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace orbit::content
{
namespace
{
[[nodiscard]] std::vector<std::byte>
EncodeBmp(
    const ThumbnailPixels& pixels)
{
    if (pixels.width == 0 ||
        pixels.height == 0)
    {
        throw std::invalid_argument(
            "Thumbnail dimensions must be non-zero.");
    }

    const u64 expected =
        static_cast<u64>(
            pixels.width) *
        static_cast<u64>(
            pixels.height) *
        4ULL;

    if (expected !=
        pixels.rgba8.size())
    {
        throw std::invalid_argument(
            "Thumbnail provider returned an invalid RGBA8 byte count.");
    }

    constexpr u32 kFileHeaderBytes = 14;
    constexpr u32 kInfoHeaderBytes = 40;

    const u64 total =
        static_cast<u64>(
            kFileHeaderBytes +
            kInfoHeaderBytes) +
        expected;

    if (total >
        std::numeric_limits<u32>::max())
    {
        throw std::overflow_error(
            "Thumbnail is too large for BMP.");
    }

    std::vector<std::byte> result(
        static_cast<std::size_t>(
            total),
        std::byte{0});

    const auto write16 =
        [&result](
            const std::size_t offset,
            const u16 value)
        {
            result[offset] =
                static_cast<std::byte>(
                    value & 0xffU);
            result[offset + 1U] =
                static_cast<std::byte>(
                    (value >> 8U) &
                    0xffU);
        };

    const auto write32 =
        [&result](
            const std::size_t offset,
            const u32 value)
        {
            for (u32 byte = 0;
                 byte < 4U;
                 ++byte)
            {
                result[
                    offset + byte] =
                    static_cast<std::byte>(
                        (value >>
                         (byte * 8U)) &
                        0xffU);
            }
        };

    write16(0, 0x4d42U);
    write32(
        2,
        static_cast<u32>(
            total));
    write32(
        10,
        kFileHeaderBytes +
            kInfoHeaderBytes);

    write32(14, kInfoHeaderBytes);
    write32(18, pixels.width);
    write32(22, pixels.height);
    write16(26, 1U);
    write16(28, 32U);
    write32(
        34,
        static_cast<u32>(
            expected));

    constexpr std::size_t
        kPixelOffset =
            kFileHeaderBytes +
            kInfoHeaderBytes;

    for (u32 outputY = 0;
         outputY < pixels.height;
         ++outputY)
    {
        const u32 sourceY =
            pixels.height -
            1U -
            outputY;

        for (u32 x = 0;
             x < pixels.width;
             ++x)
        {
            const std::size_t source =
                (static_cast<std::size_t>(
                     sourceY) *
                     pixels.width +
                 x) *
                4U;

            const std::size_t destination =
                kPixelOffset +
                (static_cast<std::size_t>(
                     outputY) *
                     pixels.width +
                 x) *
                    4U;

            result[destination + 0U] =
                pixels.rgba8[
                    source + 2U];
            result[destination + 1U] =
                pixels.rgba8[
                    source + 1U];
            result[destination + 2U] =
                pixels.rgba8[
                    source + 0U];
            result[destination + 3U] =
                pixels.rgba8[
                    source + 3U];
        }
    }

    return result;
}

[[nodiscard]] ThumbnailPixels
FallbackThumbnail(
    const ThumbnailRequest& request)
{
    ThumbnailPixels result{
        .width = request.width,
        .height = request.height,
        .rgba8 =
            std::vector<std::byte>(
                static_cast<std::size_t>(
                    request.width) *
                request.height *
                4U)
    };

    const auto& digest =
        request.sourceHash.Bytes();

    const u8 baseR =
        static_cast<u8>(
            48U +
            std::to_integer<u8>(
                digest[0]) %
                128U);
    const u8 baseG =
        static_cast<u8>(
            48U +
            std::to_integer<u8>(
                digest[1]) %
                128U);
    const u8 baseB =
        static_cast<u8>(
            48U +
            std::to_integer<u8>(
                digest[2]) %
                128U);

    const ContentHash categoryHash =
        HashString(
            request.category);

    const u8 accentR =
        static_cast<u8>(
            96U +
            std::to_integer<u8>(
                categoryHash.Bytes()[0]) %
                128U);
    const u8 accentG =
        static_cast<u8>(
            96U +
            std::to_integer<u8>(
                categoryHash.Bytes()[1]) %
                128U);
    const u8 accentB =
        static_cast<u8>(
            96U +
            std::to_integer<u8>(
                categoryHash.Bytes()[2]) %
                128U);

    for (u32 y = 0;
         y < request.height;
         ++y)
    {
        for (u32 x = 0;
             x < request.width;
             ++x)
        {
            const bool accent =
                x == y ||
                x + y + 1U ==
                    request.width ||
                ((x / 12U +
                  y / 12U) %
                     2U ==
                 0U &&
                 x > request.width / 5U &&
                 x < request.width * 4U / 5U &&
                 y > request.height / 5U &&
                 y < request.height * 4U / 5U);

            const std::size_t offset =
                (static_cast<std::size_t>(
                     y) *
                     request.width +
                 x) *
                4U;

            result.rgba8[offset + 0U] =
                static_cast<std::byte>(
                    accent
                        ? accentR
                        : baseR);
            result.rgba8[offset + 1U] =
                static_cast<std::byte>(
                    accent
                        ? accentG
                        : baseG);
            result.rgba8[offset + 2U] =
                static_cast<std::byte>(
                    accent
                        ? accentB
                        : baseB);
            result.rgba8[offset + 3U] =
                std::byte{0xff};
        }
    }

    return result;
}

[[nodiscard]] ContentHash ThumbnailKey(
    const ThumbnailRequest& request,
    const std::string_view providerId,
    const u32 providerVersion)
{
    std::string canonical =
        "orbit-thumbnail-v1\n";

    canonical +=
        request.sourceHash.ToHex();
    canonical.push_back('\n');
    canonical.append(
        providerId);
    canonical.push_back('\n');
    canonical +=
        std::to_string(
            providerVersion);
    canonical.push_back('\n');
    canonical.append(
        request.category);
    canonical.push_back('\n');
    canonical +=
        std::to_string(
            request.width);
    canonical.push_back('x');
    canonical +=
        std::to_string(
            request.height);

    return HashString(
        canonical);
}
} // namespace

ThumbnailService::ThumbnailService(
    DerivedDataCache& cache)
    : cache_(cache)
{
}

void ThumbnailService::RegisterProvider(
    ThumbnailProviderDescriptor provider)
{
    if (provider.id.empty() ||
        provider.version == 0 ||
        provider.category.empty() ||
        !provider.generate)
    {
        throw std::invalid_argument(
            "Thumbnail provider requires id, version, category and callback.");
    }

    if (FindProvider(
            provider.category) !=
        nullptr)
    {
        throw std::invalid_argument(
            "Thumbnail category already has a provider: " +
            provider.category);
    }

    providers_.push_back(
        std::move(provider));
}

const ThumbnailProviderDescriptor*
ThumbnailService::FindProvider(
    const std::string_view category) const noexcept
{
    const auto found =
        std::ranges::find_if(
            providers_,
            [category](
                const ThumbnailProviderDescriptor&
                    provider)
            {
                return provider.category ==
                    category;
            });

    return found == providers_.end()
        ? nullptr
        : &*found;
}

ThumbnailResult ThumbnailService::Get(
    const ThumbnailRequest& request)
{
    if (request.width == 0 ||
        request.height == 0)
    {
        throw std::invalid_argument(
            "Thumbnail dimensions must be non-zero.");
    }

    const u64 pixelCount =
        static_cast<u64>(
            request.width) *
        static_cast<u64>(
            request.height);

    if (pixelCount >
        static_cast<u64>(
            std::numeric_limits<std::size_t>::max() /
            4U))
    {
        throw std::overflow_error(
            "Thumbnail dimensions exceed addressable memory.");
    }

    const ThumbnailProviderDescriptor*
        provider =
            FindProvider(
                request.category);

    constexpr std::string_view
        kFallbackId =
            "orbit.fallback";
    constexpr u32
        kFallbackVersion = 1;

    const std::string_view providerId =
        provider != nullptr
            ? std::string_view(
                  provider->id)
            : kFallbackId;

    const u32 providerVersion =
        provider != nullptr
            ? provider->version
            : kFallbackVersion;

    const ContentHash key =
        ThumbnailKey(
            request,
            providerId,
            providerVersion);

    constexpr std::string_view
        kArtifactName =
            "thumbnail.bmp";

    if (cache_.Contains(
            key,
            kArtifactName))
    {
        return {
            .key = key,
            .path =
                cache_.ArtifactPath(
                    key,
                    kArtifactName),
            .width = request.width,
            .height = request.height,
            .cacheHit = true,
            .providerId =
                std::string(
                    providerId)
        };
    }

    const ThumbnailPixels pixels =
        provider != nullptr
            ? provider->generate(
                  request)
            : FallbackThumbnail(
                  request);

    const auto encoded =
        EncodeBmp(
            pixels);

    return {
        .key = key,
        .path =
            cache_.Store(
                key,
                kArtifactName,
                encoded),
        .width = pixels.width,
        .height = pixels.height,
        .cacheHit = false,
        .providerId =
            std::string(
                providerId)
    };
}
} // namespace orbit::content
