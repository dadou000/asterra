#pragma once
#include <orbit/path_geometry/CorridorSweep.hpp>
#include <string>
#include <vector>

namespace orbit::path_geometry
{
// Engineering analysis is read-only: warnings do not silently reshape authored
// geometry. Defaults are advisory, never universal road/rail standards.
enum class CorridorIssue : u8
{
    InvalidInput, ExcessiveGrade, TightRadius, ExcessiveLateralAcceleration,
    ExcessiveBank, AbruptBankTransition
};
struct CorridorEngineeringRules
{
    f64 designSpeedMetersPerSecond{20.0};
    f64 maximumGrade{0.08};
    f64 minimumRadiusMeters{0.0};
    f64 maximumLateralAccelerationMetersPerSecondSquared{2.0};
    f64 maximumBankRadians{0.15};
    f64 maximumBankGradientRadiansPerMeter{0.002};
    f64 nominalGravityMetersPerSecondSquared{9.80665};
};
struct CorridorEngineeringFinding
{
    CorridorIssue issue{CorridorIssue::InvalidInput};
    f64 stationMeters{0.0};
    f64 measured{0.0};
    f64 permitted{0.0};
    std::string explanation;
};
struct CorridorEngineeringStation
{
    f64 stationMeters{0.0};
    f64 grade{0.0};
    f64 curvaturePerMeter{0.0};
    f64 radiusMeters{0.0}; // positive infinity on a straight
    f64 lateralAccelerationMetersPerSecondSquared{0.0};
    f64 recommendedBankRadians{0.0}; // signed; banking still requires an authored decision
};
struct CorridorEngineeringReport
{
    std::vector<CorridorEngineeringStation> stations;
    std::vector<CorridorEngineeringFinding> findings;
    [[nodiscard]] bool Valid() const noexcept;
};
[[nodiscard]] CorridorEngineeringReport AnalyzeCorridorEngineering(
    const std::vector<PathStation>& stations,
    const CorridorEngineeringRules& rules,
    const std::vector<f64>& actualBankRadians = {});
// Generate bounded, transition-limited advisory bank/cant values. Caller
// explicitly decides whether to apply these to authored corridor geometry.
[[nodiscard]] std::vector<f64> SuggestCorridorBankProfile(
    const CorridorEngineeringReport& report,
    const CorridorEngineeringRules& rules);
} // namespace orbit::path_geometry
