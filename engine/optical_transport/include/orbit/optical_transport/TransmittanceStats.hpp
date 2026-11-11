#pragma once

#include <array>
#include <span>

namespace orbit::optical_transport
{

// Sufficient statistics for filtering a sub-pixel distribution of optical depth.
//
// For a footprint whose samples have unit-scale optical depth s_i, the quantity a pixel actually shows is
// T(K) = mean(exp(-K s_i)) for whatever scale K the view/sun/medium puts on it (column thickness, slant
// 1/mu, density multiplier). T(K) is a Laplace transform of the sample distribution. Two facts make it the right
// thing to store in a LOD hierarchy:
//   * every channel is a plain average, so ordinary linear mip/box/bilinear filtering of the channels is EXACT
//     (filtering s and then exponentiating is not: mean(exp(-K s)) != exp(-K mean(s)));
//   * four log-spaced samples reconstruct T(K) over three decades of K with a few percent error, where the
//     mean-only representation is wrong by up to 0.6 in transmittance for a half-covered footprint.
inline constexpr int kLaplaceChannels = 4;
inline constexpr std::array<double, kLaplaceChannels> kLaplaceScales{0.5, 2.0, 8.0, 32.0};

struct TransmittanceStats
{
    std::array<double, kLaplaceChannels> channel{1.0, 1.0, 1.0, 1.0};  // mean exp(-K_j s)
    double meanDepth = 0.0;                                            // mean s (the conserved mass)

    // Transmittance for an arbitrary scale K >= 0. Log-log interpolation of -ln T between channels, linear in K
    // below the first (T ~ 1 - K mean(s)), held at the last above it (the asymptote is the empty fraction).
    double Transmittance(double scale) const;
    double Opacity(double scale) const { return 1.0 - Transmittance(scale); }
    // -ln T(K): the optical depth a homogeneous pixel would need to show the same transmittance.
    double EffectiveDepth(double scale) const;
};

TransmittanceStats FromSamples(std::span<const double> unitDepths);

// Weighted average of footprints (weights need not be normalised). Exactly what a box-filter mip does.
TransmittanceStats Blend(std::span<const TransmittanceStats> parts, std::span<const double> weights);

} // namespace orbit::optical_transport
