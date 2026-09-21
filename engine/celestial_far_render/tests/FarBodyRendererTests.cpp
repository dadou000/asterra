#include <orbit/celestial_far_render/FarBodyRenderer.hpp>

#include <cmath>

int main()
{
    orbit::celestial_appearance::PlanetaryAppearanceProduct
        appearance;
    appearance.faceResolution = 3U;
    appearance.fingerprint = 1234U;
    appearance.texels.resize(
        6U * 3U * 3U);

    for (std::size_t index = 0;
         index < appearance.texels.size();
         ++index)
    {
        auto& texel =
            appearance.texels[index];
        texel.albedoLinear = {
            0.10F +
                static_cast<orbit::f32>(
                    index % 5U) *
                    0.01F,
            0.20F,
            0.30F
        };
        texel.roughness = 0.7F;
        texel.oceanMask =
            index % 2U == 0U
                ? 1.0F
                : 0.0F;
        texel.iceMask =
            index % 3U == 0U
                ? 1.0F
                : 0.0F;
    }

    const auto summary =
        orbit::celestial_far_render::
            SummarizeAppearance(
                appearance);

    if (!(summary.albedoLinear.x > 0.10F) ||
        std::abs(
            summary.roughness -
            0.7F) >
            1.0e-6F ||
        summary.oceanFraction <= 0.0F ||
        summary.oceanFraction >= 1.0F ||
        summary.iceFraction <= 0.0F ||
        summary.iceFraction >= 1.0F)
    {
        return 1;
    }

    const auto disc =
        orbit::celestial_far_render::
            BuildCachedDisc(
                appearance,
                {.resolution = 32U});

    if (disc.resolution != 32U ||
        disc.rgba16.size() !=
            32U * 32U * 4U ||
        disc.appearanceFingerprint !=
            appearance.fingerprint ||
        disc.fingerprint == 0U)
    {
        return 2;
    }

    const std::size_t center =
        (16U * 32U + 16U) * 4U;

    if (disc.rgba16[center + 3U] == 0U)
    {
        return 3;
    }

    if (disc.rgba16[3U] != 0U)
    {
        return 4;
    }

    appearance.fingerprint = 1235U;

    const auto revised =
        orbit::celestial_far_render::
            BuildCachedDisc(
                appearance,
                {.resolution = 32U});

    if (revised.fingerprint ==
        disc.fingerprint)
    {
        return 5;
    }

    return 0;
}
