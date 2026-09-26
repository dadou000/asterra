#include <orbit/shading/ShadingCapture.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace orbit::shading
{
namespace
{
void WriteU16(std::ostream& out, const u16 value)
{
    const std::array<char, 2> bytes{
        static_cast<char>(value & 0xFFU),
        static_cast<char>((value >> 8U) & 0xFFU)};
    out.write(bytes.data(), 2);
}

void WriteU32(std::ostream& out, const u32 value)
{
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xFFU),
        static_cast<char>((value >> 8U) & 0xFFU),
        static_cast<char>((value >> 16U) & 0xFFU),
        static_cast<char>((value >> 24U) & 0xFFU)};
    out.write(bytes.data(), 4);
}

[[nodiscard]] u32 ReadU32(std::istream& in)
{
    std::array<unsigned char, 4> bytes{};
    in.read(reinterpret_cast<char*>(bytes.data()), 4);
    if (!in)
    {
        throw std::runtime_error("Truncated capture file.");
    }
    return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8U) |
        (static_cast<u32>(bytes[2]) << 16U) |
        (static_cast<u32>(bytes[3]) << 24U);
}
} // namespace

FloatImage ReadFloatCapture(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error("Could not open " + path.string());
    }

    std::array<char, 4> magic{};
    in.read(magic.data(), 4);
    if (!in || std::string_view(magic.data(), 4) != "OFB1")
    {
        throw std::runtime_error("Not an OFB1 capture: " + path.string());
    }

    FloatImage image;
    image.width = ReadU32(in);
    image.height = ReadU32(in);
    const u32 channels = ReadU32(in);

    if (channels != 4U || image.width == 0U || image.height == 0U ||
        image.width > 16384U || image.height > 16384U)
    {
        throw std::runtime_error("Unsupported OFB1 capture layout.");
    }

    image.rgba.resize(
        static_cast<std::size_t>(image.width) * image.height * 4U);
    in.read(
        reinterpret_cast<char*>(image.rgba.data()),
        static_cast<std::streamsize>(image.rgba.size() * sizeof(f32)));
    if (!in)
    {
        throw std::runtime_error("Truncated capture file.");
    }
    return image;
}

void WriteBmp32(const std::filesystem::path& path, const FloatImage& image)
{
    if (image.width == 0U || image.height == 0U ||
        image.rgba.size() !=
            static_cast<std::size_t>(image.width) * image.height * 4U)
    {
        throw std::invalid_argument(
            "Cannot write an empty or inconsistent image.");
    }

    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path());
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        throw std::runtime_error("Could not write " + path.string());
    }

    const u32 pixelBytes = image.width * image.height * 4U;
    constexpr u32 kHeaderBytes = 14U + 40U;

    out.write("BM", 2);
    WriteU32(out, kHeaderBytes + pixelBytes);
    WriteU32(out, 0U);
    WriteU32(out, kHeaderBytes);

    WriteU32(out, 40U);
    WriteU32(out, image.width);
    // Negative height: rows are stored top-down.
    WriteU32(out, static_cast<u32>(-static_cast<i32>(image.height)));
    WriteU16(out, 1U);
    WriteU16(out, 32U);
    WriteU32(out, 0U);
    WriteU32(out, pixelBytes);
    WriteU32(out, 2835U);
    WriteU32(out, 2835U);
    WriteU32(out, 0U);
    WriteU32(out, 0U);

    const auto byte = [](const f32 value)
    {
        const f32 clamped =
            std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.0F;
        return static_cast<char>(
            static_cast<unsigned char>(std::lround(clamped * 255.0F)));
    };

    std::vector<char> row(static_cast<std::size_t>(image.width) * 4U);
    for (u32 y = 0U; y < image.height; ++y)
    {
        for (u32 x = 0U; x < image.width; ++x)
        {
            const f32* source =
                &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4U];
            char* target = &row[static_cast<std::size_t>(x) * 4U];
            target[0] = byte(source[2]); // B
            target[1] = byte(source[1]); // G
            target[2] = byte(source[0]); // R
            target[3] = static_cast<char>(0xFF);
        }
        out.write(row.data(), static_cast<std::streamsize>(row.size()));
    }
}
} // namespace orbit::shading
