#include <orbit/celestial_orbits/ImportedEphemeris.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace
{
bool Near(double a, double b, double rel = 1.0e-10)
{
    const double scale =
        std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= rel * scale;
}
} // namespace

int main()
{
    using namespace orbit::celestial_orbits;

    ImportedEphemerisProvider provider(
        {
            {
                .time = {.microsecondsFromEpoch = 10'000'000},
                .positionMeters = {10.0, 0.0, 0.0},
                .velocityMetersPerSecond = {1.0, 0.0, 0.0}
            },
            {
                .time = {.microsecondsFromEpoch = 0},
                .positionMeters = {0.0, 0.0, 0.0},
                .velocityMetersPerSecond = {1.0, 0.0, 0.0}
            }
        },
        "unit-test");

    if (provider.Range().first.microsecondsFromEpoch != 0 ||
        provider.Range().last.microsecondsFromEpoch != 10'000'000 ||
        provider.SourceName() != "unit-test")
    {
        return 1;
    }

    const auto exact =
        provider.EvaluateState(
            {.microsecondsFromEpoch = 0});

    if (exact.quality != OrbitStateQuality::SampledExact ||
        exact.positionMeters.x != 0.0 ||
        exact.velocityMetersPerSecond.x != 1.0)
    {
        return 2;
    }

    const auto middle =
        provider.EvaluateState(
            {.microsecondsFromEpoch = 5'000'000});

    if (middle.quality !=
            OrbitStateQuality::SampledInterpolated ||
        !Near(middle.positionMeters.x, 5.0) ||
        !Near(middle.velocityMetersPerSecond.x, 1.0))
    {
        return 3;
    }

    bool rejected = false;
    try
    {
        static_cast<void>(
            provider.EvaluateState(
                {.microsecondsFromEpoch = -1}));
    }
    catch (const std::out_of_range&)
    {
        rejected = true;
    }

    if (!rejected ||
        provider.ContainsTime(
            {.microsecondsFromEpoch = -1}))
    {
        return 4;
    }

    bool duplicateRejected = false;
    try
    {
        ImportedEphemerisProvider duplicate(
            {
                {.time = {.microsecondsFromEpoch = 1}},
                {.time = {.microsecondsFromEpoch = 1}}
            });
        static_cast<void>(duplicate);
    }
    catch (const std::invalid_argument&)
    {
        duplicateRejected = true;
    }

    if (!duplicateRejected)
    {
        return 5;
    }

    return 0;
}
