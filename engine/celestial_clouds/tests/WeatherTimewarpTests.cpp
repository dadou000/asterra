#include <orbit/celestial_clouds/WeatherModel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace orbit;
using namespace orbit::celestial_clouds;

namespace
{
int failures = 0;
void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

std::vector<math::Double3> FibonacciSphere(int count)
{
    std::vector<math::Double3> out;
    const double golden = std::numbers::pi * (3.0 - std::sqrt(5.0));
    for (int i = 0; i < count; ++i)
    {
        const double y = 1.0 - 2.0 * (i + 0.5) / count, r = std::sqrt(1.0 - y * y), t = golden * i;
        out.push_back({r * std::cos(t), y, r * std::sin(t)});
    }
    return out;
}
} // namespace

int main()
{
    const WeatherParameters parameters;
    const WeatherClimate climate;
    const auto directions = FibonacciSphere(6000);
    const std::vector<double> epochs{0.0, 1.0e5, 1.0e7, 3.15e7, 1.0e9, 1.0e10};   // up to ~317 years of timewarp

    // The spec's timewarp invariant: the weather at an absolute time is a pure function of position and time, valid at
    // any epoch, evaluated directly (no stepping through the skipped interval), and just as smooth in time decades away.
    bool valid = true, pure = true;
    std::vector<double> meanCoverage, changePerSecond, tropics, subtropics;
    for (const double t : epochs)
    {
        double sum = 0.0, change = 0.0, tropicSum = 0.0, subtropicSum = 0.0;
        int tropicCount = 0, subtropicCount = 0;
        for (const auto& d : directions)
        {
            const WeatherState a = EvaluateWeather(d, t, parameters, climate);
            const WeatherState again = EvaluateWeather(d, t, parameters, climate);
            const WeatherState next = EvaluateWeather(d, t + 1.0, parameters, climate);
            pure = pure && a.coverage == again.coverage && a.cloudType == again.cloudType && a.cirrus == again.cirrus && a.precipitation == again.precipitation;
            valid = valid && std::isfinite(a.coverage) && a.coverage >= 0.0 && a.coverage <= 1.0 && a.cirrus >= 0.0 && a.cirrus <= 1.0 &&
                    a.precipitation >= 0.0 && a.precipitation <= 1.0 && a.cloudType >= 0.0 && a.cloudType <= 1.0 + 1.0e-9;
            sum += a.coverage;
            change += std::abs(next.coverage - a.coverage);
            const double latitudeDegrees = std::asin(d.y) * 180.0 / std::numbers::pi;
            if (std::abs(latitudeDegrees) < 10.0) { tropicSum += a.coverage; ++tropicCount; }
            if (std::abs(latitudeDegrees) > 20.0 && std::abs(latitudeDegrees) < 35.0) { subtropicSum += a.coverage; ++subtropicCount; }
        }
        meanCoverage.push_back(sum / directions.size());
        changePerSecond.push_back(change / directions.size());
        tropics.push_back(tropicSum / tropicCount);
        subtropics.push_back(subtropicSum / subtropicCount);
    }
    Check(valid, "weather is finite and within range at every epoch up to 1e10 s");
    Check(pure, "the same position and time always gives the same weather");

    const double spreadOfMean = *std::max_element(meanCoverage.begin(), meanCoverage.end()) - *std::min_element(meanCoverage.begin(), meanCoverage.end());
    std::printf("global mean coverage by epoch:");
    for (double m : meanCoverage) std::printf(" %.3f", m);
    std::printf("  (spread %.3f)\n", spreadOfMean);
    Check(spreadOfMean < 0.04, "global cloud cover does not drift over centuries of timewarp");

    bool organised = true;
    for (std::size_t i = 0; i < epochs.size(); ++i) organised = organised && tropics[i] > subtropics[i] + 0.05;
    Check(organised, "the circulation stays organised: the tropics are cloudier than the subtropical subsidence zone at every epoch");

    const double early = changePerSecond.front();
    bool smooth = true;
    for (const double c : changePerSecond) smooth = smooth && std::abs(c - early) < 0.10 * early;
    std::printf("mean |change in coverage per second|: %.2e at t = 0, %.2e at t = 1e10 s\n", early, changePerSecond.back());
    Check(smooth, "time resolution does not degrade with the epoch (no precision loss far from t = 0)");
    Check(early > 0.0, "weather does evolve");

    // Informational: how fast the pattern decorrelates (not asserted: an authoring choice, see the weather docs).
    {
        const double base = 5.0e8;
        for (const double dt : {3600.0, 86400.0, 7.0 * 86400.0})
        {
            double sa = 0.0, sb = 0.0, sab = 0.0, saa = 0.0, sbb = 0.0;
            for (const auto& d : directions)
            {
                const double a = EvaluateWeather(d, base, parameters, climate).coverage, b = EvaluateWeather(d, base + dt, parameters, climate).coverage;
                sa += a; sb += b; sab += a * b; saa += a * a; sbb += b * b;
            }
            const double n = static_cast<double>(directions.size());
            const double correlation = (sab / n - sa / n * sb / n) / std::sqrt((saa / n - sa * sa / n / n) * (sbb / n - sb * sb / n / n));
            std::printf("pattern correlation after %6.1f hours: %.2f\n", dt / 3600.0, correlation);
        }
    }

    if (failures == 0) std::printf("WeatherTimewarp: all tests passed\n");
    return failures;
}
