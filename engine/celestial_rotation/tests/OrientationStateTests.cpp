#include <orbit/celestial_rotation/OrientationState.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

namespace
{
bool Near(double a, double b, double rel = 1.0e-8)
{
    const double scale =
        std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= rel * scale;
}
} // namespace

int main()
{
    using namespace orbit::celestial_rotation;

    const orbit::time::SimulationTime epoch{};

    const UniformSpinOrientation uniform{
        .axisInParent = {0.0, 0.0, 1.0},
        .angularVelocityRadiansPerSecond =
            std::numbers::pi_v<double> / 2.0,
        .phaseRadiansAtEpoch = 0.0,
        .epoch = epoch
    };

    const auto oneSecond =
        EvaluateUniformSpin(
            uniform,
            orbit::time::SimulationTime{
                .microsecondsFromEpoch = 1'000'000});

    if (!Near(oneSecond.parentFromBodyRotation.xAxis.x, 0.0) ||
        !Near(oneSecond.parentFromBodyRotation.xAxis.y, 1.0))
    {
        return 1;
    }

    const auto tilted =
        EvaluateUniformSpin(
            UniformSpinOrientation{
                .axisInParent = {0.0, 1.0, 1.0},
                .angularVelocityRadiansPerSecond = 0.0,
                .phaseRadiansAtEpoch = 0.0,
                .epoch = epoch
            },
            epoch);

    const double invSqrt2 =
        1.0 / std::sqrt(2.0);

    if (!Near(tilted.parentFromBodyRotation.yAxis.y, invSqrt2) ||
        !Near(tilted.parentFromBodyRotation.yAxis.z, invSqrt2))
    {
        return 4;
    }

    auto orbit =
        std::make_shared<
            orbit::celestial_orbits::AnalyticConicOrbitStateProvider>(
                orbit::celestial_orbits::AnalyticConicElements{
                    .semiMajorAxisMeters = 7.0e6,
                    .eccentricity = 0.0,
                    .gravitationalParameterM3PerS2 =
                        3.986004418e14,
                    .epoch = epoch
                });

    const SynchronousOrientation synchronous{
        .orbitState = orbit,
        .poleInParent = {0.0, 0.0, 1.0},
        .phaseOffsetRadians = 0.0
    };

    const auto syncEpoch =
        EvaluateSynchronous(
            synchronous,
            epoch);

    if (!Near(syncEpoch.parentFromBodyRotation.xAxis.x, -1.0) ||
        !Near(syncEpoch.parentFromBodyRotation.xAxis.y, 0.0) ||
        !Near(syncEpoch.parentFromBodyRotation.yAxis.z, 1.0))
    {
        return 2;
    }

    const double period =
        2.0 * std::numbers::pi_v<double> *
        std::sqrt(
            7.0e6 * 7.0e6 * 7.0e6 /
            3.986004418e14);

    const auto syncQuarter =
        EvaluateSynchronous(
            synchronous,
            orbit::time::SimulationTime{
                .microsecondsFromEpoch =
                    static_cast<orbit::i64>(
                        period * 0.25 * 1'000'000.0)
            });

    if (!Near(syncQuarter.parentFromBodyRotation.xAxis.x, 0.0, 2.0e-6) ||
        !Near(syncQuarter.parentFromBodyRotation.xAxis.y, -1.0, 2.0e-6))
    {
        return 3;
    }

    return 0;
}
