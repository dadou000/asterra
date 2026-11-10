#include "orbit/storm_electrification/FlashSchedule.hpp"

#include <algorithm>
#include <cmath>

namespace orbit::storm_electrification
{

namespace
{
std::uint64_t Mix(std::uint64_t z)
{
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
double Unit(std::uint64_t z) { return static_cast<double>(Mix(z) >> 11) * (1.0 / 9007199254740992.0); }
}

FlashSchedule::FlashSchedule(std::uint64_t seed, std::vector<RateKnot> knots) : seed_(seed), knots_(std::move(knots))
{
    if (knots_.empty()) knots_.push_back({});
    std::sort(knots_.begin(), knots_.end(), [](const RateKnot& a, const RateKnot& b) { return a.time < b.time; });
    cumulative_.assign(knots_.size(), 0.0);
    for (std::size_t i = 1; i < knots_.size(); ++i)
    {
        cumulative_[i] = cumulative_[i - 1] + 0.5 * (knots_[i - 1].rate + knots_[i].rate) * (knots_[i].time - knots_[i - 1].time);
    }
}

double FlashSchedule::ExpectedCount(double t) const
{
    if (t <= knots_.front().time) return knots_.front().rate * (t - knots_.front().time);
    if (t >= knots_.back().time) return cumulative_.back() + knots_.back().rate * (t - knots_.back().time);
    std::size_t i = 1;
    while (knots_[i].time < t) ++i;
    const double dt = knots_[i].time - knots_[i - 1].time;
    const double u = (t - knots_[i - 1].time);
    const double slope = dt > 0.0 ? (knots_[i].rate - knots_[i - 1].rate) / dt : 0.0;
    return cumulative_[i - 1] + knots_[i - 1].rate * u + 0.5 * slope * u * u;
}

double FlashSchedule::Invert(double intensity) const
{
    // Bisection on the monotone ExpectedCount; the rate is non-negative so the count never decreases.
    double lo = knots_.front().time - 1.0, hi = knots_.back().time + 1.0;
    while (ExpectedCount(lo) > intensity) lo -= (hi - lo);
    while (ExpectedCount(hi) < intensity)
    {
        if (knots_.back().rate <= 0.0) return hi;   // no more flashes ever
        hi += (hi - lo);
    }
    for (int i = 0; i < 80; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        (ExpectedCount(mid) < intensity ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

std::vector<double> FlashSchedule::FlashesIn(double t0, double t1) const
{
    std::vector<double> out;
    if (!(t1 > t0)) return out;
    const double a = ExpectedCount(t0), b = ExpectedCount(t1);
    const long first = static_cast<long>(std::floor(a)), last = static_cast<long>(std::floor(b));
    for (long n = first; n <= last; ++n)
    {
        // Poisson(1) count by inversion of the CDF with a hashed uniform.
        const double u = Unit(seed_ ^ Mix(static_cast<std::uint64_t>(n) * 2ULL + 1ULL));
        int count = 0;
        double term = std::exp(-1.0), cdf = term;
        while (u > cdf && count < 12) { ++count; term /= count; cdf += term; }
        for (int e = 0; e < count; ++e)
        {
            const double position = static_cast<double>(n) + Unit(seed_ ^ Mix(static_cast<std::uint64_t>(n) * 977ULL + static_cast<std::uint64_t>(e) * 31ULL + 7ULL));
            if (position < a || position >= b) continue;
            out.push_back(Invert(position));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace orbit::storm_electrification
