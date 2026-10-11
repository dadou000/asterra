#include "orbit/optical_transport/StellarVisibility.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbit::optical_transport
{

double AngularRadius(double radius, double distance)
{
    return std::asin(std::clamp(radius / distance, 0.0, 1.0));
}

double PenumbraWidth(double separation, double angularRadius)
{
    return 2.0 * separation * std::tan(angularRadius);
}

namespace
{
// Integral of the limb-darkened disk intensity over the part with x < c (disk of unit radius), normalised to 1.
double DiskCdf(double c, double u)
{
    constexpr int steps = 256;
    c = std::clamp(c, -1.0, 1.0);
    double total = 0.0;
    double part = 0.0;
    const double dx = 2.0 / steps;
    for (int i = 0; i < steps; ++i)
    {
        const double x = -1.0 + (i + 0.5) * dx;
        const double h = std::sqrt(std::max(0.0, 1.0 - x * x));
        // Integrate over y: mu = sqrt(1 - x^2 - y^2); I = 1 - u (1 - mu).
        constexpr int ys = 32;
        double column = 0.0;
        const double dy = 2.0 * h / ys;
        for (int j = 0; j < ys && h > 0.0; ++j)
        {
            const double y = -h + (j + 0.5) * dy;
            const double mu = std::sqrt(std::max(0.0, 1.0 - x * x - y * y));
            column += (1.0 - u * (1.0 - mu)) * dy;
        }
        total += column * dx;
        if (x < c)
        {
            part += column * dx;
        }
    }
    return total > 0.0 ? part / total : 0.0;
}
} // namespace

double DiskVisibilityAcrossEdge(double edgeOffset, double angularRadius, double limbDarkening)
{
    if (angularRadius <= 0.0)
    {
        return edgeOffset > 0.0 ? 0.0 : 1.0;
    }
    // The blocker hides everything on one side of the edge; visible part is x < edge (normalised).
    const double c = -edgeOffset / angularRadius;
    if (c <= -1.0) return 0.0;
    if (c >= 1.0) return 1.0;
    return DiskCdf(c, limbDarkening);
}

double DiskTransmission(double angularRadius, double limbDarkening, int samplesPerAxis,
                        const std::function<double(double, double)>& opticalDepthAt)
{
    double weighted = 0.0;
    double weights = 0.0;
    const int n = std::max(samplesPerAxis, 1);
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            const double x = -1.0 + (2.0 * i + 1.0) / n;
            const double y = -1.0 + (2.0 * j + 1.0) / n;
            const double r2 = x * x + y * y;
            if (r2 > 1.0) continue;
            const double mu = std::sqrt(1.0 - r2);
            const double w = 1.0 - limbDarkening * (1.0 - mu);
            weighted += w * std::exp(-opticalDepthAt(x * angularRadius, y * angularRadius));
            weights += w;
        }
    }
    return weights > 0.0 ? weighted / weights : 1.0;
}

TransmissionStats FilterOpticalDepths(std::span<const double> taus, double coverageThreshold)
{
    TransmissionStats stats;
    if (taus.empty()) return stats;
    double t = 0.0, tau = 0.0;
    std::size_t covered = 0;
    for (const double v : taus)
    {
        t += std::exp(-v);
        tau += v;
        covered += v > coverageThreshold ? 1u : 0u;
    }
    const double inv = 1.0 / static_cast<double>(taus.size());
    stats.meanTransmission = t * inv;
    stats.meanOpticalDepth = tau * inv;
    stats.coverage = static_cast<double>(covered) * inv;
    return stats;
}

double EffectiveOpticalDepth(const TransmissionStats& stats)
{
    return -std::log(std::max(stats.meanTransmission, 1.0e-300));
}

double HorizonVisibility(double starElevation, double horizonElevation, double angularRadius, double limbDarkening)
{
    // Elevation of the disk centre above the blocking horizon; the edge crosses the disk when |offset| < radius.
    const double offset = horizonElevation - starElevation;  // >0: horizon above the centre
    return DiskVisibilityAcrossEdge(offset, angularRadius, limbDarkening);
}

} // namespace orbit::optical_transport
