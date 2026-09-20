#pragma once

#include <orbit/world_model/PropertyProvenance.hpp>

#include <optional>
#include <string>
#include <vector>

namespace orbit::world_model
{
struct ScalarPhysicalProperty
{
    std::optional<f64> value;
    PropertyProvenance provenance;
};

enum class SolveOutcome : u8
{
    NoChange = 0,
    Derived = 1,
    Conflict = 2,
    InvalidInput = 3
};

struct SolveEvent
{
    std::string target;
    std::vector<std::string> dependencies;
    SolveOutcome outcome{SolveOutcome::NoChange};
    std::string explanation;
};

struct PhysicalSolveReport
{
    std::vector<SolveEvent> events;

    [[nodiscard]] bool HasConflict() const noexcept;
    [[nodiscard]] bool HasInvalidInput() const noexcept;
};

class PhysicalPropertySolver
{
public:
    // Spherical mean-density relation:
    // rho = m / (4/3 pi r^3). Exactly one writable/missing member may be
    // solved per call; explicit/imported/locked values are never overwritten.
    [[nodiscard]] PhysicalSolveReport SolveMassRadiusDensity(
        ScalarPhysicalProperty& massKg,
        ScalarPhysicalProperty& radiusMeters,
        ScalarPhysicalProperty& densityKgPerM3) const;

    [[nodiscard]] PhysicalSolveReport SolveEquatorialVelocity(
        const ScalarPhysicalProperty& radiusMeters,
        const ScalarPhysicalProperty& rotationPeriodSeconds,
        ScalarPhysicalProperty& equatorialVelocityMetersPerSecond) const;

    [[nodiscard]] PhysicalSolveReport SolveOrbitalPeriod(
        const ScalarPhysicalProperty& semiMajorAxisMeters,
        const ScalarPhysicalProperty& gravitationalParameterM3PerS2,
        ScalarPhysicalProperty& orbitalPeriodSeconds) const;

    [[nodiscard]] PhysicalSolveReport SolveStellarRadius(
        const ScalarPhysicalProperty& luminosityWatts,
        const ScalarPhysicalProperty& effectiveTemperatureKelvin,
        ScalarPhysicalProperty& radiusMeters) const;
};

inline constexpr f64 kStefanBoltzmannConstant =
    5.670374419e-8;
} // namespace orbit::world_model
