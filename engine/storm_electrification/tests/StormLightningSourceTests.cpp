#include <orbit/storm_electrification/StormLightningSource.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

using namespace orbit::storm_electrification;

namespace
{
int failures = 0;
void Check(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

StormFields MakeStorm(const ChargeGrid& g, double peakUpdraft)
{
    StormFields f;
    const std::size_t n = g.CellCount();
    f.temperatureK.resize(n); f.airDensity.resize(n); f.updraft.resize(n);
    f.graupel.resize(n); f.ice.resize(n); f.liquid.resize(n);
    const double cx = g.Nx() * 0.5 * g.Spacing(), cy = g.Ny() * 0.5 * g.Spacing();
    for (std::uint32_t k = 0; k < g.Nz(); ++k)
        for (std::uint32_t j = 0; j < g.Ny(); ++j)
            for (std::uint32_t i = 0; i < g.Nx(); ++i)
            {
                const std::size_t c = g.Index(i, j, k);
                const double z = g.Height(k);
                const double r = std::hypot((i + 0.5) * g.Spacing() - cx, (j + 0.5) * g.Spacing() - cy);
                const double column = std::exp(-r * r / (2.0 * 4000.0 * 4000.0));
                const double vertical = std::sin(std::numbers::pi * std::clamp((z - 500.0) / 12000.0, 0.0, 1.0));
                const double temperature = 300.0 - 6.5e-3 * z;
                f.temperatureK[c] = static_cast<float>(temperature);
                f.airDensity[c] = static_cast<float>(1.2 * std::exp(-z / 8400.0));
                f.updraft[c] = static_cast<float>(peakUpdraft * column * vertical);
                const bool cold = temperature < 268.0, veryCold = temperature < 253.0;
                const double strength = peakUpdraft / 25.0;
                f.graupel[c] = static_cast<float>(cold ? 2.0e-3 * column * strength : 0.0);
                f.ice[c] = static_cast<float>((cold ? 1.0e-3 : 0.0) * std::exp(-r * r / (2.0 * 6000.0 * 6000.0)) * (veryCold ? 2.0 : 1.0) * strength);
                const double liquidFraction = std::clamp((temperature - 253.0) / (273.15 - 253.0), 0.0, 1.0);
                f.liquid[c] = static_cast<float>(1.5e-3 * column * std::min(1.0, strength) * liquidFraction);
            }
    return f;
}

StormLightningSource Make()
{
    const ChargeGrid shape(40, 40, 28, 600.0);
    return StormLightningSource(MakeStorm(shape, 30.0), 40, 40, 28, 600.0);
}
} // namespace

int main()
{
    // A storm needs minutes of charging before its first flash.
    StormLightningSource source = Make();
    Check(source.Advance(1.0).empty() && source.FlashCount() == 0, "no flash in the first 20 simulated seconds");

    // Many small calls equal one large call (whole steps, remainder carried).
    StormLightningSource one = Make(), many = Make();
    const auto big = one.Advance(120.0);
    std::size_t manyCount = 0;
    for (int i = 0; i < 480; ++i) { manyCount += many.Advance(0.25).size(); }
    std::printf("120 s wall at 20x: %zu flashes in one call, %zu in 480 calls, ratio %.2f\n", big.size(), manyCount, one.MaxBreakdownRatio());
    Check(!big.empty(), "a strong storm flashes within 40 simulated minutes");
    Check(big.size() == manyCount && one.SimulatedSeconds() == many.SimulatedSeconds(), "call granularity does not change the result");

    // A bounded call does bounded work and drops the surplus instead of owing it to the next call.
    {
        StormLightningSource bounded = Make();
        (void)bounded.Advance(10.0, 4);   // 200 simulated seconds due, 4 steps allowed
        Check(bounded.SimulatedSeconds() == 20.0, "maxSteps bounds the work of one call");
        (void)bounded.Advance(0.25, 4);   // one step due now; no backlog from before
        Check(bounded.SimulatedSeconds() == 25.0, "the dropped backlog is not repaid later");
    }

    // Flashes lie inside the domain, carry energy, and are time-ordered.
    const double size = one.DomainSizeMeters();
    bool inside = true, ordered = true, energetic = true;
    double last = 0.0;
    for (const auto& f : big)
    {
        for (const auto& s : f.graph.Segments())
            for (int a = 0; a < 3; ++a)
            {
                const double hi = a == 2 ? 28 * 600.0 : size;
                inside = inside && s.from[a] >= -1.0 && s.from[a] <= hi + 1.0 && s.to[a] >= -1.0 && s.to[a] <= hi + 1.0;
            }
        ordered = ordered && f.startTime >= last;
        last = f.startTime;
        energetic = energetic && f.graph.EnergyInWindow(-1.0, 1.0e9) > 0.0;
    }
    Check(inside, "channels stay inside the storm domain");
    Check(ordered, "flashes come out in time order");
    Check(energetic, "every flash radiates");
    std::printf(failures == 0 ? "StormLightningSource tests passed\n" : "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
