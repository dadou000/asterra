#include "orbit/optical_transport/Refraction.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbit::optical_transport
{

namespace
{
constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = 0.5 * std::numbers::pi;

double Index(const RefractionAtmosphere& a, double r) { return 1.0 + RefractivityAt(a, r - a.planetRadius); }

// Central angle swept between radii ra < rb by a ray with invariant k, using r = ra + t^2 so the 1/sqrt singularity of
// tan(z) at a turning point (or at a grazing start) becomes smooth.
double CentralAngle(const RefractionAtmosphere& a, double k, double ra, double rb)
{
    if (rb <= ra) return 0.0;
    constexpr int steps = 1200;
    const double tMax = std::sqrt(rb - ra);
    double sum = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        const double t = (i + 0.5) * tMax / steps;
        const double r = ra + t * t;
        const double s = std::min(k / (Index(a, r) * r), 1.0 - 1.0e-15);
        const double tanz = s / std::sqrt(1.0 - s * s);
        sum += tanz / r * 2.0 * t * (tMax / steps);
    }
    return sum;
}

// Refraction of an upward or grazing-downward ray for an observer at radius r0 whose apparent zenith distance is z0.
// Returns false if the ray would hit the ground. truthZenith = zenith distance of the airless direction.
bool TraceFromObserver(const RefractionAtmosphere& a, double r0, double z0, double& truthZenith)
{
    const double rTop = a.planetRadius + 16.0 * a.scaleHeight;
    const double n0 = Index(a, r0);
    const double k = n0 * r0 * std::sin(z0);
    double theta = 0.0;
    double rStart = r0;
    if (z0 > kHalfPi)
    {
        // Descending: find the turning radius where n r = k (n r is increasing with r).
        double lo = a.planetRadius, hi = r0;
        if (Index(a, lo) * lo > k) return false;   // reaches the ground
        for (int i = 0; i < 80; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            (Index(a, mid) * mid < k ? lo : hi) = mid;
        }
        const double rMin = 0.5 * (lo + hi);
        theta += CentralAngle(a, k, rMin, r0);
        rStart = rMin;
    }
    theta += CentralAngle(a, k, rStart, rTop);
    const double sTop = std::min(k / (Index(a, rTop) * rTop), 1.0);
    const double zTop = std::asin(sTop);
    truthZenith = zTop + theta;
    return true;
}
} // namespace

double RefractivityAt(const RefractionAtmosphere& a, double h)
{
    return a.surfaceRefractivity * std::exp(-std::max(h, 0.0) / a.scaleHeight);
}

double HorizonRefractionEstimate(const RefractionAtmosphere& a)
{
    return a.surfaceRefractivity * std::sqrt(kPi * a.planetRadius / (2.0 * a.scaleHeight));
}

double SurfaceRefractivityFromSolver(double numberDensityRatio, double rayleighCompositionRatio, double reference)
{
    return reference * numberDensityRatio * std::sqrt(std::max(rayleighCompositionRatio, 0.0));
}

double CriticalGradientRatio(double gradient, double planetRadius) { return gradient / (-1.0 / planetRadius); }

double RefractionTable::GeometricDip(const RefractionAtmosphere& a, double h)
{
    return std::acos(a.planetRadius / (a.planetRadius + std::max(h, 0.0)));
}

bool RefractionTable::Ducting(const RefractionAtmosphere& a)
{
    return CriticalGradientRatio(-a.surfaceRefractivity / a.scaleHeight, a.planetRadius) >= 1.0;
}

RefractionTable RefractionTable::Build(const RefractionAtmosphere& atmosphere, double maxObserverAltitude)
{
    RefractionTable table;
    table.atmosphere_ = atmosphere;
    if (Ducting(atmosphere)) return table;   // rays can be trapped at the surface: no valid single-valued table
    // Altitudes: 0 then log-spaced from 1 m.
    std::vector<double> altitudes{0.0};
    for (double h = 1.0; h < maxObserverAltitude * 1.0001; h *= 3.1622776601683795) altitudes.push_back(h);
    if (altitudes.back() < maxObserverAltitude) altitudes.push_back(maxObserverAltitude);

    for (const double h : altitudes)
    {
        Row row;
        row.altitude = h;
        const double r0 = atmosphere.planetRadius + h;
        // Apparent zenith distances from the zenith down to the limb: dense near the horizon, where it matters.
        // The grazing ray has invariant k = n(R) R (sin 90 deg); at the observer sin z = k / (n0 r0) and the ray is descending.
        const double zLimb = kPi - std::asin(std::min(1.0, atmosphere.planetRadius * Index(atmosphere, atmosphere.planetRadius) /
                                                               (r0 * Index(atmosphere, r0))));
        std::vector<double> zs;
        constexpr int count = 220;
        for (int i = 0; i <= count; ++i)
        {
            // Map u in [0,1] to a zenith distance with density concentrated near z = 90 degrees and the limb.
            const double u = static_cast<double>(i) / count;
            const double z = (u < 0.5)
                ? kHalfPi * (1.0 - std::pow(1.0 - 2.0 * u, 2.0))              // zenith (0) ... horizon (90 deg)
                : kHalfPi + (zLimb - kHalfPi) * std::pow(2.0 * u - 1.0, 0.5); // horizon ... limb
            zs.push_back(std::min(z, zLimb));
        }
        for (const double z : zs)
        {
            double truthZ = 0.0;
            if (!TraceFromObserver(atmosphere, r0, z, truthZ)) continue;
            row.apparent.push_back(kHalfPi - z);
            row.truth.push_back(kHalfPi - truthZ);
        }
        // Order ascending in elevation.
        std::reverse(row.apparent.begin(), row.apparent.end());
        std::reverse(row.truth.begin(), row.truth.end());
        table.rows_.push_back(std::move(row));
    }
    return table;
}

std::size_t RefractionTable::RowBelow(double altitude, double& fraction) const
{
    if (altitude <= rows_.front().altitude) { fraction = 0.0; return 0; }
    if (altitude >= rows_.back().altitude) { fraction = 0.0; return rows_.size() - 1; }
    std::size_t i = 1;
    while (rows_[i].altitude < altitude) ++i;
    // Interpolate in log altitude (rows are log-spaced above the first).
    const double lo = std::max(rows_[i - 1].altitude, 0.3162), hi = rows_[i].altitude;
    fraction = std::clamp((std::log(std::max(altitude, 0.3162)) - std::log(lo)) / (std::log(hi) - std::log(lo)), 0.0, 1.0);
    return i - 1;
}

bool RefractionTable::Lookup(const Row& row, double value, bool fromApparent, double& out) const
{
    const std::vector<double>& key = fromApparent ? row.apparent : row.truth;
    const std::vector<double>& val = fromApparent ? row.truth : row.apparent;
    if (key.size() < 2 || value < key.front() || value > key.back())
    {
        if (!key.empty() && value > key.back())
        {
            out = val.back() + (value - key.back());   // above the table (beyond the zenith sample): no bending left
            return true;
        }
        return false;
    }
    const auto it = std::upper_bound(key.begin(), key.end(), value);
    const std::size_t i = static_cast<std::size_t>(std::distance(key.begin(), it));
    const std::size_t a = i == 0 ? 0 : i - 1, b = std::min(i, key.size() - 1);
    const double span = key[b] - key[a];
    const double u = span > 0.0 ? (value - key[a]) / span : 0.0;
    out = val[a] + (val[b] - val[a]) * u;
    return true;
}

bool RefractionTable::ApparentElevation(double trueElevation, double observerAltitude, double& apparent) const
{
    if (rows_.empty()) return false;
    double f = 0.0;
    const std::size_t i = RowBelow(observerAltitude, f);
    double a0 = 0.0, a1 = 0.0;
    if (!Lookup(rows_[i], trueElevation, false, a0)) return false;
    if (f <= 0.0 || i + 1 >= rows_.size()) { apparent = a0; return true; }
    if (!Lookup(rows_[i + 1], trueElevation, false, a1)) { apparent = a0; return true; }
    apparent = a0 + (a1 - a0) * f;
    return true;
}

bool RefractionTable::TrueElevation(double apparentElevation, double observerAltitude, double& trueElev) const
{
    if (rows_.empty()) return false;
    double f = 0.0;
    const std::size_t i = RowBelow(observerAltitude, f);
    double t0 = 0.0, t1 = 0.0;
    if (!Lookup(rows_[i], apparentElevation, true, t0)) return false;
    if (f <= 0.0 || i + 1 >= rows_.size()) { trueElev = t0; return true; }
    if (!Lookup(rows_[i + 1], apparentElevation, true, t1)) { trueElev = t0; return true; }
    trueElev = t0 + (t1 - t0) * f;
    return true;
}

double RefractionTable::Bending(double trueElevation, double observerAltitude) const
{
    double apparent = 0.0;
    return ApparentElevation(trueElevation, observerAltitude, apparent) ? apparent - trueElevation : 0.0;
}

double RefractionTable::VerticalCompression(double trueElevation, double observerAltitude) const
{
    constexpr double h = 2.0e-5;   // about 4 arcseconds
    double lo = 0.0, hi = 0.0;
    if (!ApparentElevation(trueElevation - h, observerAltitude, lo) || !ApparentElevation(trueElevation + h, observerAltitude, hi)) return 1.0;
    return (hi - lo) / (2.0 * h);
}

double RefractionTable::ApparentLimbElevation(double observerAltitude) const
{
    if (rows_.empty()) return -GeometricDip(atmosphere_, observerAltitude);
    double f = 0.0;
    const std::size_t i = RowBelow(observerAltitude, f);
    const double a0 = rows_[i].apparent.front();
    if (f <= 0.0 || i + 1 >= rows_.size()) return a0;
    return a0 + (rows_[i + 1].apparent.front() - a0) * f;
}

} // namespace orbit::optical_transport
