#pragma once

#include <algorithm>
#include <cmath>

namespace orbit::terrain_bake
{
// Airy-isostasy correction for deviations from reference continental/oceanic
// crustal thickness. This is a reduced-order bake contribution, not a mantle
// dynamics model. Seafloor thermal subsidence and fault relief remain owned
// by the existing tectonic structure generator.
struct IsostaticFoundationSample
{
    double continentalFraction{0.0};
    double crustThicknessKm{7.0};
};

[[nodiscard]] inline double IsostaticCrustCorrectionMeters(
    const IsostaticFoundationSample sample) noexcept
{
    if (!std::isfinite(sample.continentalFraction) ||
        !std::isfinite(sample.crustThicknessKm))
    {
        return 0.0;
    }
    const double continental = std::clamp(sample.continentalFraction, 0.0, 1.0);
    // Mixed crust is treated as an effective column. Reference thickness
    // prevents a constant offset that would arbitrarily shift sea level.
    const double referenceKm = 7.0 + 28.0 * continental;
    const double crustDensity = 2950.0 + (2750.0 - 2950.0) * continental;
    constexpr double mantleDensity = 3300.0;
    const double buoyancy = 1.0 - crustDensity / mantleDensity;
    // No exaggerated topography from malformed or extreme input.
    const double thickeningKm =
        std::clamp(sample.crustThicknessKm - referenceKm, -25.0, 40.0);
    return thickeningKm * 1000.0 * buoyancy;
}
} // namespace orbit::terrain_bake
