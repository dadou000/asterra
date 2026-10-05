#pragma once

#include <orbit/celestial_atmosphere/Atmosphere.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <array>

namespace orbit::celestial_atmosphere
{
// The sky-view table is a function of direction in a *sky frame*: +Z is the
// observer's zenith and the sun sits at azimuth 0. `SkyViewInput::sunDirectionBody`
// must therefore be the sun expressed in that frame, not a body-fixed direction.
// This builds that direction from the observer's up and the body-fixed sun, with
// the cosine of the zenith angle snapped to `cosineQuantum` so that walking over
// the surface does not change the sky-view fingerprint every frame.
[[nodiscard]] math::Double3 SkyFrameSunDirection(
    const math::Double3& observerUpBody,
    const math::Double3& sunDirectionBody,
    f64 cosineQuantum = 1.0 / 512.0);

// Basis that carries sky-frame vectors into the body frame: +Z is the observer
// zenith, +X the horizontal direction toward the sun (any horizontal direction
// when the sun is at the zenith or nadir) and +Y completes a right-handed set.
struct SkyFrameBasis
{
    math::Double3 x{1.0, 0.0, 0.0};
    math::Double3 y{0.0, 1.0, 0.0};
    math::Double3 z{0.0, 0.0, 1.0};

    [[nodiscard]] math::Double3 ToBody(
        const math::Double3& skyFrameVector) const noexcept;
    [[nodiscard]] math::Double3 FromBody(
        const math::Double3& bodyVector) const noexcept;
};

[[nodiscard]] SkyFrameBasis MakeSkyFrameBasis(
    const math::Double3& observerUpBody,
    const math::Double3& sunDirectionBody);

// Second-order real spherical-harmonic projection of the sky radiance, in the
// sky frame. Coefficients are radiance (the sky-view table's own units) per
// steradian-weighted basis function; use EvaluateSkyIrradiance for irradiance.
struct SkySphericalHarmonics
{
    std::array<math::Float3, 9> coefficients{};
};

[[nodiscard]] SkySphericalHarmonics ProjectSkyViewToSphericalHarmonics(
    const AtmosphereLut2D& skyView);

// Cosine-convolved irradiance (radiance units x steradian) arriving at a
// surface whose normal is `normalSkyFrame`. Negative lobes from the order-2
// truncation are clamped to zero.
[[nodiscard]] math::Float3 EvaluateSkyIrradiance(
    const SkySphericalHarmonics& sh,
    const math::Double3& normalSkyFrame) noexcept;
} // namespace orbit::celestial_atmosphere
