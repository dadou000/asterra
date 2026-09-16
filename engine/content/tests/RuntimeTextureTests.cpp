#include <orbit/content/RuntimeTexture.hpp>

#include <array>
#include <cstddef>
#include <iostream>
#include <span>

#define ORBIT_TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            std::cerr \
                << "RuntimeTexture test failed: " \
                << #expression \
                << " at line " \
                << __LINE__ \
                << '\n'; \
            return 1; \
        } \
    } while (false)

int main()
{
    const std::array<std::byte, 8>
        pixels{
            std::byte{0x10},
            std::byte{0x20},
            std::byte{0x30},
            std::byte{0xff},
            std::byte{0x40},
            std::byte{0x50},
            std::byte{0x60},
            std::byte{0x80}
        };

    const auto encoded =
        orbit::content::
            EncodeRuntimeTextureRgba8(
                2,
                1,
                std::span(
                    pixels.data(),
                    pixels.size()));

    const auto decoded =
        orbit::content::
            DecodeRuntimeTexture(
                std::span(
                    encoded.data(),
                    encoded.size()));

    ORBIT_TEST_CHECK(
        decoded.width == 2);
    ORBIT_TEST_CHECK(
        decoded.height == 1);
    ORBIT_TEST_CHECK(
        decoded.format ==
        orbit::content::
            RuntimeTextureFormat::
                Rgba8Unorm);
    ORBIT_TEST_CHECK(
        decoded.pixels.size() ==
        pixels.size());

    for (std::size_t index = 0;
         index < pixels.size();
         ++index)
    {
        ORBIT_TEST_CHECK(
            decoded.pixels[index] ==
            pixels[index]);
    }

    return 0;
}
