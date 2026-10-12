#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace orbit::optical_transport
{
// Linear sRGB colour of a blackbody at `kelvin`, scaled so that its largest component is 1 (a chromaticity: the brightness is the caller's).
// The Planckian locus is the cubic approximation of Kim et al. (2002) for 1667 K to 25000 K; hotter temperatures keep the chromaticity of
// 25000 K (the blue-white limit, the locus changes very little above it) and cooler ones that of 1667 K (deep orange).
//
// A lightning channel is a plasma with line and continuum emission, not a blackbody: this is the colour of a body at the channel's estimated
// temperature, used as the best available stand-in (hot channel blue-white, cooling channel white then orange).
[[nodiscard]] inline std::array<double, 3> BlackbodyLinearRgb(double kelvin) noexcept
{
    const double t = std::clamp(kelvin, 1667.0, 25000.0);
    const double t2 = t * t, t3 = t2 * t;
    const double x = t <= 4000.0 ? -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910
                                 : -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
    const double x2 = x * x, x3 = x2 * x;
    double y;
    if (t <= 2222.0) { y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683; }
    else if (t <= 4000.0) { y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867; }
    else { y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483; }
    const double bigY = 1.0;
    const double bigX = x / y * bigY;
    const double bigZ = (1.0 - x - y) / y * bigY;
    double r = 3.2406 * bigX - 1.5372 * bigY - 0.4986 * bigZ;
    double g = -0.9689 * bigX + 1.8758 * bigY + 0.0415 * bigZ;
    double b = 0.0557 * bigX - 0.2040 * bigY + 1.0570 * bigZ;
    r = std::max(r, 0.0);
    g = std::max(g, 0.0);
    b = std::max(b, 0.0);
    const double peak = std::max({r, g, b, 1.0e-9});
    return {r / peak, g / peak, b / peak};
}
} // namespace orbit::optical_transport
