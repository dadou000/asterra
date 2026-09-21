#include <orbit/post_process/ColorLut.hpp>

#include <cassert>
#include <cstddef>

int main()
{
    const auto lut =
        orbit::post_process::BuildIdentityColorLut(4U);

    assert(lut.size == 4U);
    assert(lut.rgba8.size() ==
        static_cast<std::size_t>(4U * 4U * 4U * 4U));

    const auto sample =
        [&lut](const orbit::u32 r,
               const orbit::u32 g,
               const orbit::u32 b,
               const orbit::u32 channel)
        {
            const orbit::u32 x =
                b * lut.size + r;
            const orbit::u32 y = g;
            const std::size_t offset =
                (static_cast<std::size_t>(y) *
                     lut.size * lut.size +
                 x) *
                4U;
            return lut.rgba8[offset + channel];
        };

    assert(sample(0U, 0U, 0U, 0U) == 0U);
    assert(sample(3U, 0U, 0U, 0U) == 255U);
    assert(sample(0U, 3U, 0U, 1U) == 255U);
    assert(sample(0U, 0U, 3U, 2U) == 255U);
    assert(sample(3U, 3U, 3U, 3U) == 255U);

    return 0;
}
