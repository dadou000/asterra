#include <orbit/world_model/PhysicalPropertySolver.hpp>

#include <cmath>

namespace
{
bool Near(const double a, const double b, const double rel = 1.0e-8)
{
    const double scale = std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= rel * scale;
}

orbit::world_model::ScalarPhysicalProperty Explicit(const double value)
{
    using namespace orbit::world_model;
    return {
        .value = value,
        .provenance = {
            .sourceMode = PropertySourceMode::Explicit,
            .solveState = PropertySolveState::Locked
        }
    };
}

orbit::world_model::ScalarPhysicalProperty Writable()
{
    return {};
}
} // namespace

int main()
{
    using namespace orbit::world_model;

    PhysicalPropertySolver solver;

    auto mass = Explicit(5.9722e24);
    auto radius = Explicit(6.371e6);
    auto density = Writable();

    const auto densityReport =
        solver.SolveMassRadiusDensity(mass, radius, density);

    if (!density.value.has_value() ||
        !Near(*density.value, 5513.4, 5.0e-4) ||
        density.provenance.sourceMode != PropertySourceMode::Derived ||
        density.provenance.solveState != PropertySolveState::Solved ||
        densityReport.events.empty())
    {
        return 1;
    }

    auto explicitBadDensity = Explicit(1000.0);
    const auto conflictReport =
        solver.SolveMassRadiusDensity(
            mass, radius, explicitBadDensity);

    if (!conflictReport.HasConflict() ||
        *explicitBadDensity.value != 1000.0)
    {
        return 2;
    }

    auto rotationPeriod = Explicit(86164.0905);
    auto equatorialVelocity = Writable();
    const auto rotationReport =
        solver.SolveEquatorialVelocity(
            radius,
            rotationPeriod,
            equatorialVelocity);

    if (!equatorialVelocity.value.has_value() ||
        !Near(*equatorialVelocity.value, 464.6, 5.0e-3) ||
        rotationReport.events.empty())
    {
        return 3;
    }

    auto axis = Explicit(149597870700.0);
    auto solarMu = Explicit(1.32712440018e20);
    auto orbitalPeriod = Writable();
    const auto orbitReport =
        solver.SolveOrbitalPeriod(
            axis, solarMu, orbitalPeriod);

    if (!orbitalPeriod.value.has_value() ||
        !Near(*orbitalPeriod.value, 31558196.0, 5.0e-5) ||
        orbitReport.events.empty())
    {
        return 4;
    }

    auto luminosity = Explicit(3.828e26);
    auto temperature = Explicit(5772.0);
    auto stellarRadius = Writable();
    const auto starReport =
        solver.SolveStellarRadius(
            luminosity, temperature, stellarRadius);

    if (!stellarRadius.value.has_value() ||
        !Near(*stellarRadius.value, 6.957e8, 2.0e-3) ||
        starReport.events.empty())
    {
        return 5;
    }

    auto invalidPeriod = Explicit(0.0);
    auto invalidVelocity = Writable();
    if (!solver.SolveEquatorialVelocity(
            radius,
            invalidPeriod,
            invalidVelocity).HasInvalidInput())
    {
        return 6;
    }

    return 0;
}
