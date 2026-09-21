#include <orbit/lighting/PlanetaryEmissionAppearance.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] math::Double3 FaceDirection(
    const u32 face,
    const f64 u,
    const f64 v)
{
    math::Double3 p{};

    switch (face)
    {
    case 0: p = { 1.0, v, -u}; break;
    case 1: p = {-1.0, v,  u}; break;
    case 2: p = { u, 1.0, -v}; break;
    case 3: p = { u,-1.0,  v}; break;
    case 4: p = { u, v, 1.0}; break;
    default:p = {-u, v,-1.0}; break;
    }

    return math::Normalize(p);
}

[[nodiscard]] u64 Mix(
    const u64 a,
    const u64 b) noexcept
{
    return
        a ^
        (b +
         0x9e3779b97f4a7c15ULL +
         (a << 6U) +
         (a >> 2U));
}
} // namespace

void ApplyPlanetaryEmissionField(
    celestial_appearance::PlanetaryAppearanceProduct& appearance,
    const PlanetaryEmissionField& field,
    const f64 referenceRadiusMeters,
    const u32 level,
    const f32 intensityScale)
{
    if (appearance.faceResolution < 3U ||
        appearance.texels.empty() ||
        level >= field.levels.size() ||
        !std::isfinite(referenceRadiusMeters) ||
        referenceRadiusMeters <= 0.0 ||
        !std::isfinite(intensityScale) ||
        intensityScale < 0.0F)
    {
        throw std::invalid_argument(
            "Planetary emission appearance overlay is invalid.");
    }

    const std::size_t faceStride =
        static_cast<std::size_t>(
            appearance.faceResolution) *
        appearance.faceResolution;

    if (appearance.texels.size() !=
        faceStride * 6U)
    {
        throw std::invalid_argument(
            "Planetary appearance texel count does not match cube layout.");
    }

    for (u32 face = 0U;
         face < 6U;
         ++face)
    {
        for (u32 y = 0U;
             y < appearance.faceResolution;
             ++y)
        {
            const f64 v =
                -1.0 +
                2.0 *
                static_cast<f64>(y) /
                static_cast<f64>(
                    appearance.faceResolution -
                    1U);

            for (u32 x = 0U;
                 x < appearance.faceResolution;
                 ++x)
            {
                const f64 u =
                    -1.0 +
                    2.0 *
                    static_cast<f64>(x) /
                    static_cast<f64>(
                        appearance.faceResolution -
                        1U);

                const auto direction =
                    FaceDirection(
                        face,
                        u,
                        v);

                const auto radiance =
                    EvaluatePlanetaryEmissionRadiance(
                        field,
                        level,
                        direction,
                        referenceRadiusMeters);

                const std::size_t index =
                    static_cast<std::size_t>(face) *
                        faceStride +
                    static_cast<std::size_t>(y) *
                        appearance.faceResolution +
                    x;

                appearance.texels[index].
                    emissionLinear = {
                        appearance.texels[index].
                                emissionLinear.x +
                            radiance.x *
                                intensityScale,
                        appearance.texels[index].
                                emissionLinear.y +
                            radiance.y *
                                intensityScale,
                        appearance.texels[index].
                                emissionLinear.z +
                            radiance.z *
                                intensityScale
                    };
            }
        }
    }

    appearance.fingerprint =
        Mix(
            Mix(
                appearance.fingerprint,
                field.sourceRevision),
            static_cast<u64>(level) +
                0x4d3139454d495353ULL);
}
} // namespace orbit::lighting
