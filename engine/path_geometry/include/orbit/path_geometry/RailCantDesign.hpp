#pragma once
#include <orbit/path_geometry/CorridorEngineering.hpp>
#include <vector>

namespace orbit::path_geometry
{
// Rail-specific cant is vertical rail-height difference in metres. This is
// intentionally distinct from generic road banking in radians.
struct RailCantLimits
{
    f64 gaugeMeters{1.435};
    f64 designSpeedMetersPerSecond{22.0};
    f64 minimumOperatingSpeedMetersPerSecond{10.0};
    f64 maximumOperatingSpeedMetersPerSecond{28.0};
    f64 maximumCantMeters{0.150};
    f64 maximumCantDeficiencyMeters{0.100};
    f64 maximumCantExcessMeters{0.075};
    f64 maximumCantGradient{0.002}; // metres cant / metre of path
    f64 gravityMetersPerSecondSquared{9.80665};
};
enum class RailCantIssue : u8
{
    InvalidInput, IncompatibleSpeeds, ExcessCantDeficiency,
    ExcessCantAtSlowSpeed
};
struct RailCantFinding
{
    RailCantIssue issue{RailCantIssue::InvalidInput};
    f64 stationMeters{0.0};
    f64 measuredMeters{0.0};
    f64 permittedMeters{0.0};
};
struct RailCantStation
{
    f64 stationMeters{0.0};
    f64 equilibriumAtMinimumSpeedMeters{0.0};
    f64 equilibriumAtMaximumSpeedMeters{0.0};
    f64 suggestedCantMeters{0.0};
    f64 suggestedBankRadians{0.0};
    f64 remainingDeficiencyMeters{0.0};
    f64 remainingExcessMeters{0.0};
};
struct RailCantPlan
{
    std::vector<RailCantStation> stations;
    std::vector<RailCantFinding> findings;
    [[nodiscard]] bool Valid() const noexcept;
};
// Pure read-only advisory design. Limits are configurable for the fictional
// network and are not asserted as any country's railway standard.
[[nodiscard]] RailCantPlan DesignRailCant(
    const CorridorEngineeringReport& alignment,
    const RailCantLimits& limits = {});
} // namespace orbit::path_geometry
