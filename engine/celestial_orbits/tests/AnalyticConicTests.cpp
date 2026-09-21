#include <orbit/celestial_orbits/OrbitState.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace
{
bool Near(
    const double a,
    const double b,
    const double relative = 1.0e-8)
{
    const double scale =
        std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <=
        relative * scale;
}
} // namespace

int main()
{
    using namespace orbit::celestial_orbits;

    const orbit::time::SimulationTime epoch{
        .microsecondsFromEpoch = 0
    };

    const AnalyticConicElements circular{
        .semiMajorAxisMeters = 7.0e6,
        .eccentricity = 0.0,
        .gravitationalParameterM3PerS2 = 3.986004418e14,
        .epoch = epoch
    };

    const auto circularEpoch =
        EvaluateAnalyticConic(circular, epoch);

    if (!Near(circularEpoch.positionMeters.x, 7.0e6) ||
        !Near(circularEpoch.positionMeters.y, 0.0) ||
        !Near(
            circularEpoch.velocityMetersPerSecond.y,
            std::sqrt(3.986004418e14 / 7.0e6)))
    {
        return 1;
    }

    const double period =
        2.0 * std::numbers::pi_v<double> *
        std::sqrt(
            std::pow(7.0e6, 3.0) /
            3.986004418e14);

    const orbit::time::SimulationTime quarter{
        .microsecondsFromEpoch =
            static_cast<orbit::i64>(
                period * 0.25 * 1'000'000.0)
    };

    const auto circularQuarter =
        EvaluateAnalyticConic(circular, quarter);

    if (!Near(circularQuarter.positionMeters.x, 0.0, 2.0e-6) ||
        !Near(circularQuarter.positionMeters.y, 7.0e6, 2.0e-6))
    {
        return 2;
    }

    const AnalyticConicElements ellipse{
        .semiMajorAxisMeters = 10.0e6,
        .eccentricity = 0.5,
        .gravitationalParameterM3PerS2 = 3.986004418e14,
        .epoch = epoch
    };

    const auto periapsis =
        EvaluateAnalyticConic(ellipse, epoch);

    if (!Near(periapsis.positionMeters.x, 5.0e6))
    {
        return 3;
    }

    const AnalyticConicElements parabola{
        .periapsisDistanceMeters = 2.0e6,
        .eccentricity = 1.0,
        .gravitationalParameterM3PerS2 = 3.986004418e14,
        .epoch = epoch
    };

    const auto parabolicPeriapsis =
        EvaluateAnalyticConic(parabola, epoch);

    if (!Near(parabolicPeriapsis.positionMeters.x, 2.0e6) ||
        !Near(parabolicPeriapsis.positionMeters.y, 0.0) ||
        !Near(
            parabolicPeriapsis.velocityMetersPerSecond.y,
            std::sqrt(
                2.0 * 3.986004418e14 / 2.0e6)))
    {
        return 4;
    }

    const AnalyticConicElements hyperbola{
        .semiMajorAxisMeters = 4.0e6,
        .eccentricity = 1.5,
        .gravitationalParameterM3PerS2 = 3.986004418e14,
        .epoch = epoch
    };

    const auto hyperbolicPeriapsis =
        EvaluateAnalyticConic(hyperbola, epoch);

    if (!Near(
            hyperbolicPeriapsis.positionMeters.x,
            2.0e6))
    {
        return 5;
    }

    const AnalyticConicElements inclined{
        .semiMajorAxisMeters = 7.0e6,
        .eccentricity = 0.0,
        .inclinationRadians =
            std::numbers::pi_v<double> / 2.0,
        .argumentPeriapsisRadians =
            std::numbers::pi_v<double> / 2.0,
        .gravitationalParameterM3PerS2 = 3.986004418e14,
        .epoch = epoch
    };

    const auto rotated =
        EvaluateAnalyticConic(inclined, epoch);

    if (!Near(rotated.positionMeters.z, 7.0e6))
    {
        return 6;
    }

    const FixedOrbitState fixed{
        .positionMeters = {1.0, 2.0, 3.0}
    };
    const auto fixedState =
        EvaluateFixed(fixed, quarter);

    if (fixedState.positionMeters.x != 1.0 ||
        fixedState.velocityMetersPerSecond.x != 0.0 ||
        fixedState.quality != OrbitStateQuality::Fixed)
    {
        return 7;
    }

    return 0;
}
