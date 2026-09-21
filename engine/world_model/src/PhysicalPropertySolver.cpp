#include <orbit/world_model/PhysicalPropertySolver.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>

namespace orbit::world_model
{
namespace
{
constexpr f64 kRelativeTolerance = 1.0e-10;

[[nodiscard]] bool FinitePositive(
    const ScalarPhysicalProperty& property) noexcept
{
    return property.value.has_value() &&
           std::isfinite(*property.value) &&
           *property.value > 0.0;
}

[[nodiscard]] bool NearlyEqual(
    const f64 a,
    const f64 b) noexcept
{
    const f64 scale =
        std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <=
        kRelativeTolerance * scale;
}

void CommitDerived(
    ScalarPhysicalProperty& target,
    const f64 value,
    std::string sourceProperty)
{
    target.value = value;
    target.provenance.sourceMode =
        PropertySourceMode::Derived;
    target.provenance.solveState =
        PropertySolveState::Solved;
    target.provenance.sourceProperty =
        std::move(sourceProperty);
    target.provenance.diagnostic.clear();
}

void RecordPrediction(
    PhysicalSolveReport& report,
    ScalarPhysicalProperty& target,
    const f64 predicted,
    std::string targetName,
    std::vector<std::string> dependencies,
    std::string explanation)
{
    if (!std::isfinite(predicted) || predicted <= 0.0)
    {
        report.events.push_back({
            .target = std::move(targetName),
            .dependencies = std::move(dependencies),
            .outcome = SolveOutcome::InvalidInput,
            .explanation = "Derived value is non-finite or non-positive."
        });
        return;
    }

    if (CanSolverWrite(target.provenance))
    {
        if (!target.value.has_value() ||
            !NearlyEqual(*target.value, predicted))
        {
            CommitDerived(
                target,
                predicted,
                explanation);

            report.events.push_back({
                .target = std::move(targetName),
                .dependencies = std::move(dependencies),
                .outcome = SolveOutcome::Derived,
                .explanation = std::move(explanation)
            });
        }
        return;
    }

    if (target.value.has_value() &&
        !NearlyEqual(*target.value, predicted))
    {
        report.events.push_back({
            .target = std::move(targetName),
            .dependencies = std::move(dependencies),
            .outcome = SolveOutcome::Conflict,
            .explanation =
                "Authoritative target disagrees with the physical constraint; value was not overwritten."
        });
    }
}
} // namespace

bool PhysicalSolveReport::HasConflict() const noexcept
{
    for (const auto& event : events)
    {
        if (event.outcome == SolveOutcome::Conflict)
        {
            return true;
        }
    }
    return false;
}

bool PhysicalSolveReport::HasInvalidInput() const noexcept
{
    for (const auto& event : events)
    {
        if (event.outcome == SolveOutcome::InvalidInput)
        {
            return true;
        }
    }
    return false;
}

PhysicalSolveReport
PhysicalPropertySolver::SolveMassRadiusDensity(
    ScalarPhysicalProperty& massKg,
    ScalarPhysicalProperty& radiusMeters,
    ScalarPhysicalProperty& densityKgPerM3) const
{
    PhysicalSolveReport report;

    const bool massValid = FinitePositive(massKg);
    const bool radiusValid = FinitePositive(radiusMeters);
    const bool densityValid = FinitePositive(densityKgPerM3);

    const int validCount =
        static_cast<int>(massValid) +
        static_cast<int>(radiusValid) +
        static_cast<int>(densityValid);

    if (validCount < 2)
    {
        report.events.push_back({
            .target = "mass/radius/density",
            .dependencies = {},
            .outcome = SolveOutcome::InvalidInput,
            .explanation =
                "At least two positive finite values are required."
        });
        return report;
    }

    constexpr f64 fourThirdsPi =
        (4.0 / 3.0) * std::numbers::pi_v<f64>;

    if (massValid && radiusValid)
    {
        const f64 predictedDensity =
            *massKg.value /
            (fourThirdsPi *
             std::pow(*radiusMeters.value, 3.0));

        RecordPrediction(
            report,
            densityKgPerM3,
            predictedDensity,
            "density",
            {"mass", "radius"},
            "density = mass / ((4/3) * pi * radius^3)");
    }

    if (radiusValid && densityValid)
    {
        const f64 predictedMass =
            *densityKgPerM3.value *
            fourThirdsPi *
            std::pow(*radiusMeters.value, 3.0);

        RecordPrediction(
            report,
            massKg,
            predictedMass,
            "mass",
            {"radius", "density"},
            "mass = density * (4/3) * pi * radius^3");
    }

    if (massValid && densityValid)
    {
        const f64 predictedRadius =
            std::cbrt(
                *massKg.value /
                (*densityKgPerM3.value * fourThirdsPi));

        RecordPrediction(
            report,
            radiusMeters,
            predictedRadius,
            "radius",
            {"mass", "density"},
            "radius = cbrt(mass / (density * (4/3) * pi))");
    }

    return report;
}

PhysicalSolveReport
PhysicalPropertySolver::SolveEquatorialVelocity(
    const ScalarPhysicalProperty& radiusMeters,
    const ScalarPhysicalProperty& rotationPeriodSeconds,
    ScalarPhysicalProperty& equatorialVelocityMetersPerSecond) const
{
    PhysicalSolveReport report;

    if (!FinitePositive(radiusMeters) ||
        !FinitePositive(rotationPeriodSeconds))
    {
        report.events.push_back({
            .target = "equatorial velocity",
            .dependencies = {"radius", "rotation period"},
            .outcome = SolveOutcome::InvalidInput,
            .explanation =
                "Radius and rotation period must be positive and finite."
        });
        return report;
    }

    const f64 predicted =
        (2.0 * std::numbers::pi_v<f64> *
         *radiusMeters.value) /
        *rotationPeriodSeconds.value;

    RecordPrediction(
        report,
        equatorialVelocityMetersPerSecond,
        predicted,
        "equatorial velocity",
        {"radius", "rotation period"},
        "equatorial velocity = 2 * pi * radius / rotation period");

    return report;
}

PhysicalSolveReport
PhysicalPropertySolver::SolveOrbitalPeriod(
    const ScalarPhysicalProperty& semiMajorAxisMeters,
    const ScalarPhysicalProperty& gravitationalParameterM3PerS2,
    ScalarPhysicalProperty& orbitalPeriodSeconds) const
{
    PhysicalSolveReport report;

    if (!FinitePositive(semiMajorAxisMeters) ||
        !FinitePositive(gravitationalParameterM3PerS2))
    {
        report.events.push_back({
            .target = "orbital period",
            .dependencies = {"semi-major axis", "gravitational parameter"},
            .outcome = SolveOutcome::InvalidInput,
            .explanation =
                "Semi-major axis and gravitational parameter must be positive and finite."
        });
        return report;
    }

    const f64 predicted =
        2.0 * std::numbers::pi_v<f64> *
        std::sqrt(
            std::pow(*semiMajorAxisMeters.value, 3.0) /
            *gravitationalParameterM3PerS2.value);

    RecordPrediction(
        report,
        orbitalPeriodSeconds,
        predicted,
        "orbital period",
        {"semi-major axis", "gravitational parameter"},
        "orbital period = 2 * pi * sqrt(semi-major axis^3 / mu)");

    return report;
}

PhysicalSolveReport
PhysicalPropertySolver::SolveStellarRadius(
    const ScalarPhysicalProperty& luminosityWatts,
    const ScalarPhysicalProperty& effectiveTemperatureKelvin,
    ScalarPhysicalProperty& radiusMeters) const
{
    PhysicalSolveReport report;

    if (!FinitePositive(luminosityWatts) ||
        !FinitePositive(effectiveTemperatureKelvin))
    {
        report.events.push_back({
            .target = "stellar radius",
            .dependencies = {"luminosity", "effective temperature"},
            .outcome = SolveOutcome::InvalidInput,
            .explanation =
                "Luminosity and effective temperature must be positive and finite."
        });
        return report;
    }

    const f64 temperature4 =
        std::pow(*effectiveTemperatureKelvin.value, 4.0);
    const f64 predicted =
        std::sqrt(
            *luminosityWatts.value /
            (4.0 * std::numbers::pi_v<f64> *
             kStefanBoltzmannConstant *
             temperature4));

    RecordPrediction(
        report,
        radiusMeters,
        predicted,
        "stellar radius",
        {"luminosity", "effective temperature"},
        "stellar radius = sqrt(luminosity / (4 * pi * sigma * temperature^4))");

    return report;
}
} // namespace orbit::world_model
