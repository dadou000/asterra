#include <orbit/content/RuntimeTexture.hpp>

#include <array>
#include <limits>
#include <stdexcept>

namespace orbit::content
{
namespace
{
constexpr std::array<std::byte, 8>
    kMagic{
        std::byte{'O'},
        std::byte{'R'},
        std::byte{'B'},
        std::byte{'T'},
        std::byte{'E'},
        std::byte{'X'},
        std::byte{'0'},
        std::byte{'1'}
    };

void AppendU32(
    std::vector<std::byte>& output,
    const u32 value)
{
    for (u32 shift = 0;
         shift < 32U;
         shift += 8U)
    {
        output.push_back(
            static_cast<std::byte>(
                (value >> shift) &
                0xffU));
    }
}

void AppendU64(
    std::vector<std::byte>& output,
    const u64 value)
{
    for (u32 shift = 0;
         shift < 64U;
         shift += 8U)
    {
        output.push_back(
            static_cast<std::byte>(
                (value >> shift) &
                0xffULL));
    }
}

[[nodiscard]] u32 ReadU32(
    const std::span<const std::byte> bytes,
    const std::size_t offset)
{
    if (offset + 4U >
        bytes.size())
    {
        throw std::runtime_error(
            "Orbit texture header is truncated.");
    }

    u32 value = 0;

    for (u32 index = 0;
         index < 4U;
         ++index)
    {
        value |=
            static_cast<u32>(
                std::to_integer<u8>(
                    bytes[offset +
                          index]))
            << (index * 8U);
    }

    return value;
}

[[nodiscard]] u64 ReadU64(
    const std::span<const std::byte> bytes,
    const std::size_t offset)
{
    if (offset + 8U >
        bytes.size())
    {
        throw std::runtime_error(
            "Orbit texture header is truncated.");
    }

    u64 value = 0;

    for (u32 index = 0;
         index < 8U;
         ++index)
    {
        value |=
            static_cast<u64>(
                std::to_integer<u8>(
                    bytes[offset +
                          index]))
            << (index * 8U);
    }

    return value;
}

[[nodiscard]] u64 ExpectedPixelBytes(
    const u32 width,
    const u32 height)
{
    if (width == 0 ||
        height == 0)
    {
        throw std::invalid_argument(
            "Orbit texture dimensions must be non-zero.");
    }

    constexpr u64 bytesPerPixel = 4;

    const u64 pixels =
        static_cast<u64>(
            width) *
        static_cast<u64>(
            height);

    if (pixels >
        std::numeric_limits<u64>::max() /
            bytesPerPixel)
    {
        throw std::overflow_error(
            "Orbit texture dimensions overflow.");
    }

    return pixels *
        bytesPerPixel;
}
} // namespace

std::vector<std::byte>
EncodeRuntimeTextureRgba8(
    const u32 width,
    const u32 height,
    const std::span<const std::byte> pixels)
{
    const u64 expected =
        ExpectedPixelBytes(
            width,
            height);

    if (pixels.size() !=
        expected)
    {
        throw std::invalid_argument(
            "RGBA8 texture payload size does not match dimensions.");
    }

    constexpr std::size_t
        kHeaderBytes = 32;

    std::vector<std::byte> result;
    result.reserve(
        kHeaderBytes +
        pixels.size());

    result.insert(
        result.end(),
        kMagic.begin(),
        kMagic.end());

    AppendU32(
        result,
        kRuntimeTextureVersion);
    AppendU32(
        result,
        width);
    AppendU32(
        result,
        height);
    AppendU32(
        result,
        static_cast<u32>(
            RuntimeTextureFormat::
                Rgba8Unorm));
    AppendU64(
        result,
        expected);

    result.insert(
        result.end(),
        pixels.begin(),
        pixels.end());

    return result;
}

RuntimeTexture DecodeRuntimeTexture(
    const std::span<const std::byte> bytes)
{
    constexpr std::size_t
        kHeaderBytes = 32;

    if (bytes.size() <
        kHeaderBytes)
    {
        throw std::runtime_error(
            "Orbit texture file is smaller than its header.");
    }

    for (std::size_t index = 0;
         index < kMagic.size();
         ++index)
    {
        if (bytes[index] !=
            kMagic[index])
        {
            throw std::runtime_error(
                "Orbit texture magic is invalid.");
        }
    }

    const u32 version =
        ReadU32(
            bytes,
            8);

    if (version !=
        kRuntimeTextureVersion)
    {
        throw std::runtime_error(
            "Unsupported Orbit texture version.");
    }

    RuntimeTexture result{
        .width =
            ReadU32(
                bytes,
                12),
        .height =
            ReadU32(
                bytes,
                16),
        .format =
            static_cast<
                RuntimeTextureFormat>(
                    ReadU32(
                        bytes,
                        20))
    };

    if (result.format !=
        RuntimeTextureFormat::
            Rgba8Unorm)
    {
        throw std::runtime_error(
            "Unsupported Orbit runtime texture format.");
    }

    const u64 payloadBytes =
        ReadU64(
            bytes,
            24);

    const u64 expected =
        ExpectedPixelBytes(
            result.width,
            result.height);

    if (payloadBytes !=
            expected ||
        bytes.size() -
                kHeaderBytes !=
            expected)
    {
        throw std::runtime_error(
            "Orbit texture payload size is invalid.");
    }

    result.pixels.assign(
        bytes.begin() +
            static_cast<
                std::ptrdiff_t>(
                    kHeaderBytes),
        bytes.end());

    return result;
}
} // namespace orbit::content
