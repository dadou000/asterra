#pragma once

#include <cstdint>
#include <vector>

namespace orbit::storm_electrification
{

// Deterministic, random-access flash times for an inhomogeneous Poisson process, so extreme timewarp can ask "which
// flashes happen in [t0, t1]?" at absolute simulation time without simulating the skipped interval.
//
// The rate is a piecewise-linear table of flashes per second. The cumulative intensity L(t) is split into unit strata;
// stratum n holds Poisson(1) events at positions uniform in intensity, both derived by hashing (seed, n). Disjoint
// strata are independent, so this IS a Poisson process with rate(t), and any window only touches its own strata.
struct RateKnot
{
    double time = 0.0;      // s
    double rate = 0.0;      // flashes per second at this time (linear between knots, held outside)
};

class FlashSchedule
{
public:
    FlashSchedule(std::uint64_t seed, std::vector<RateKnot> knots);

    // Cumulative expected flashes from the first knot's time.
    double ExpectedCount(double t) const;
    // Flash times in [t0, t1), ascending, identical however a long window is split.
    std::vector<double> FlashesIn(double t0, double t1) const;

private:
    double Invert(double intensity) const;   // time at which ExpectedCount reaches `intensity`
    std::uint64_t seed_;
    std::vector<RateKnot> knots_;
    std::vector<double> cumulative_;         // expected count at each knot
};

} // namespace orbit::storm_electrification
