#include <orbit/post_process/ColorLut.hpp>

#include <cassert>
#include <cmath>
#include <cstddef>
#include <string>

namespace
{
orbit::u8 Sample(
    const orbit::post_process::ColorLutData& lut,
    const orbit::u32 r,
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
}
} // namespace

int main()
{
    using namespace orbit::post_process;

    const auto identity =
        BuildIdentityColorLut(4U);

    assert(identity.size == 4U);
    assert(identity.rgba8.size() ==
        static_cast<std::size_t>(4U * 4U * 4U * 4U));
    assert(IsDisplayLutCompatible(identity));
    assert(identity.metadata.explicitOrbitMetadata);

    assert(Sample(identity, 0U, 0U, 0U, 0U) == 0U);
    assert(Sample(identity, 3U, 0U, 0U, 0U) == 255U);
    assert(Sample(identity, 0U, 3U, 0U, 1U) == 255U);
    assert(Sample(identity, 0U, 0U, 3U, 2U) == 255U);
    assert(Sample(identity, 3U, 3U, 3U, 3U) == 255U);

    const std::string cube =
        "# ORBIT_DOMAIN DISPLAY_LINEAR\n"
        "# ORBIT_SHAPER NONE\n"
        "TITLE \"Axis Test\"\n"
        "LUT_3D_SIZE 2\n"
        "DOMAIN_MIN 0 0 0\n"
        "DOMAIN_MAX 1 1 1\n"
        "0 0 0\n"
        "1 0 0\n"
        "0 1 0\n"
        "1 1 0\n"
        "0 0 1\n"
        "1 0 1\n"
        "0 1 1\n"
        "1 1 1\n";

    const auto imported =
        ParseCubeColorLut(cube);

    assert(imported.lut.size == 2U);
    assert(imported.lut.metadata.title == "Axis Test");
    assert(imported.lut.metadata.domain ==
        ColorLutDomain::DisplayLinear);
    assert(imported.lut.metadata.shaper ==
        ColorLutShaper::None);
    assert(IsDisplayLutCompatible(imported.lut));

    // Confirms R-fastest/G/B .cube ordering is remapped to Orbit's packed
    // blue-slice-X / green-Y texture layout correctly.
    assert(Sample(imported.lut, 1U, 0U, 0U, 0U) == 255U);
    assert(Sample(imported.lut, 0U, 1U, 0U, 1U) == 255U);
    assert(Sample(imported.lut, 0U, 0U, 1U, 2U) == 255U);

    const auto roundTrip =
        ParseCubeColorLut(
            imported.canonicalCube);

    assert(roundTrip.lut.size ==
        imported.lut.size);
    assert(roundTrip.lut.rgba8 ==
        imported.lut.rgba8);
    assert(SerializeCubeColorLut(
               roundTrip.lut) ==
           roundTrip.canonicalCube);

    assert(std::abs(
               BlendColorLutChannel(
                   0.25F,
                   0.75F,
                   0.0F) -
               0.25F) <
           1.0e-6F);

    assert(std::abs(
               BlendColorLutChannel(
                   0.25F,
                   0.75F,
                   1.0F) -
               0.75F) <
           1.0e-6F);

    auto incompatible =
        imported.lut;
    incompatible.metadata.domain =
        ColorLutDomain::ShapedSceneLinear;
    incompatible.metadata.shaper =
        ColorLutShaper::Log2;

    assert(!IsDisplayLutCompatible(
        incompatible));

    return 0;
}
