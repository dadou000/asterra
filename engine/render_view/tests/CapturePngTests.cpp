#include <orbit/render_view/Capture.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace
{
int g_failures = 0;

void Expect(const bool condition, const char* what)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << what << "\n";
        ++g_failures;
    }
}
} // namespace

int main()
{
    namespace rv = orbit::render_view;

    // 131 x 67: not a multiple of the encoder's row chunk, odd width.
    constexpr unsigned width = 131U;
    constexpr unsigned height = 67U;
    std::vector<orbit::u8> rgba(static_cast<std::size_t>(width) * height * 4U);
    for (unsigned y = 0; y < height; ++y)
    {
        for (unsigned x = 0; x < width; ++x)
        {
            orbit::u8* p = rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4U;
            p[0] = static_cast<orbit::u8>(x * 2U);
            p[1] = static_cast<orbit::u8>(y * 3U);
            p[2] = static_cast<orbit::u8>((x + y) % 251U);
            p[3] = 0U; // a zero alpha must not make the PNG transparent
        }
    }

    const auto directory = std::filesystem::temp_directory_path() / "orbit_capture_png_tests";
    std::filesystem::remove_all(directory);
    const auto path = directory / "nested" / "shot.png";

    const rv::CaptureResult result =
        rv::WriteImageRgba8(path, width, height, rgba.data());
    Expect(result.width == width && result.height == height, "size reported");
    Expect(result.fileBytes > 0U && std::filesystem::exists(path), "file written");

    {
        std::ifstream file(path, std::ios::binary);
        char signature[8] = {};
        file.read(signature, 8);
        const char expected[8] = {
            static_cast<char>(0x89), 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        Expect(std::equal(signature, signature + 8, expected), "PNG signature");
    }

    const rv::CapturedImage back = rv::ReadImageRgba8(path);
    Expect(back.width == width && back.height == height, "size round trip");
    bool same = back.rgba.size() == rgba.size();
    for (std::size_t i = 0; same && i < rgba.size(); i += 4U)
    {
        same = back.rgba[i] == rgba[i] && back.rgba[i + 1U] == rgba[i + 1U] &&
            back.rgba[i + 2U] == rgba[i + 2U] && back.rgba[i + 3U] == 255U;
    }
    Expect(same, "pixels round trip, opaque");

    // .bmp is still honoured.
    const auto bmp = directory / "shot.bmp";
    static_cast<void>(rv::WriteImageRgba8(bmp, width, height, rgba.data()));
    {
        std::ifstream file(bmp, std::ios::binary);
        char magic[2] = {};
        file.read(magic, 2);
        Expect(magic[0] == 'B' && magic[1] == 'M', ".bmp extension writes a BMP");
    }

    std::filesystem::remove_all(directory);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
