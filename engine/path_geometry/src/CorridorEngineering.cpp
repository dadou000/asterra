#include <orbit/path_geometry/CorridorEngineering.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace orbit::path_geometry
{
CorridorEngineeringRules EngineeringRulesForProfile(
    const paths::PathProfile& profile,
    CorridorEngineeringRules defaults)
{
    defaults.minimumRadiusMeters = profile.minimumRadiusMeters;
    defaults.maximumGrade = profile.maximumGrade;
    return defaults;
}

bool CorridorEngineeringReport::Valid() const noexcept
{
    for (const auto& finding : findings)
        if (finding.issue == CorridorIssue::InvalidInput) return false;
    return true;
}

CorridorEngineeringReport AnalyzeCorridorEngineering(
    const std::vector<PathStation>& stations,
    const CorridorEngineeringRules& rules,
    const std::vector<f64>& actualBankRadians)
{
    CorridorEngineeringReport out;
    const auto warn = [&](CorridorIssue issue, f64 station,
                          f64 measured, f64 limit, const char* explanation)
    {
        out.findings.push_back({issue, station, measured, limit, explanation});
    };
    if (!std::isfinite(rules.designSpeedMetersPerSecond) ||
        rules.designSpeedMetersPerSecond < 0.0 ||
        !std::isfinite(rules.maximumGrade) || rules.maximumGrade < 0.0 ||
        !std::isfinite(rules.minimumRadiusMeters) || rules.minimumRadiusMeters < 0.0 ||
        !std::isfinite(rules.maximumLateralAccelerationMetersPerSecondSquared) ||
        rules.maximumLateralAccelerationMetersPerSecondSquared <= 0.0 ||
        !std::isfinite(rules.maximumBankRadians) || rules.maximumBankRadians < 0.0 ||
        !std::isfinite(rules.maximumBankGradientRadiansPerMeter) ||
        rules.maximumBankGradientRadiansPerMeter < 0.0 ||
        !std::isfinite(rules.nominalGravityMetersPerSecondSquared) ||
        rules.nominalGravityMetersPerSecondSquared <= 0.0 ||
        (!actualBankRadians.empty() && actualBankRadians.size() != stations.size()))
    {
        warn(CorridorIssue::InvalidInput, 0.0, 0.0, 0.0, "Invalid engineering limits or bank count.");
        return out;
    }
    for (std::size_t i = 0; i < stations.size(); ++i)
    {
        const auto& s = stations[i];
        const auto finite = [](const math::Double3& p) {
            return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
        };
        if (!std::isfinite(s.stationMeters) || !finite(s.position) ||
            !finite(s.tangent) || !finite(s.up) ||
            math::LengthSquared(s.tangent) < 1.e-12 ||
            math::LengthSquared(s.up) < 1.e-12 ||
            (i && !(s.stationMeters > stations[i-1].stationMeters)) ||
            (!actualBankRadians.empty() && !std::isfinite(actualBankRadians[i])))
        {
            warn(CorridorIssue::InvalidInput, s.stationMeters, 0.0, 0.0,
                 "Nonfinite, degenerate or non-increasing corridor station.");
            return out;
        }
    }
    out.stations.reserve(stations.size());
    for (std::size_t i = 0; i < stations.size(); ++i)
    {
        const auto& s = stations[i];
        CorridorEngineeringStation value;
        value.stationMeters = s.stationMeters;
        value.radiusMeters = std::numeric_limits<f64>::infinity();
        if (i)
        {
            const auto& prev = stations[i-1];
            const f64 distance = s.stationMeters - prev.stationMeters;
            const auto delta = s.position - prev.position;
            const auto up = math::Normalize(s.up + prev.up);
            if (math::LengthSquared(up) < 1.e-12)
            {
                warn(CorridorIssue::InvalidInput, s.stationMeters, 0.0, 0.0,
                     "Opposing station up vectors.");
                return out;
            }
            value.grade = math::Dot(delta, up) / distance;
            if (std::abs(value.grade) > rules.maximumGrade)
                warn(CorridorIssue::ExcessiveGrade, s.stationMeters,
                     std::abs(value.grade), rules.maximumGrade, "Corridor exceeds the configured grade.");
            const auto a = math::Normalize(prev.tangent);
            const auto b = math::Normalize(s.tangent);
            const f64 cosine = std::clamp(math::Dot(a,b), -1.0, 1.0);
            const f64 angle = std::acos(cosine);
            const f64 sign = math::Dot(math::Cross(a,b), up) < 0.0 ? -1.0 : 1.0;
            value.curvaturePerMeter = sign * angle / distance;
            if (angle > 1.e-9)
            {
                value.radiusMeters = distance / angle;
                if (value.radiusMeters < rules.minimumRadiusMeters)
                    warn(CorridorIssue::TightRadius, s.stationMeters,
                         value.radiusMeters, rules.minimumRadiusMeters, "Curve is tighter than the configured radius.");
            }
            value.lateralAccelerationMetersPerSecondSquared =
                rules.designSpeedMetersPerSecond * rules.designSpeedMetersPerSecond *
                std::abs(value.curvaturePerMeter);
            if (value.lateralAccelerationMetersPerSecondSquared >
                rules.maximumLateralAccelerationMetersPerSecondSquared)
                warn(CorridorIssue::ExcessiveLateralAcceleration, s.stationMeters,
                     value.lateralAccelerationMetersPerSecondSquared,
                     rules.maximumLateralAccelerationMetersPerSecondSquared,
                     "Curve exceeds the configured lateral acceleration.");
            value.recommendedBankRadians = std::clamp(
                std::atan(rules.designSpeedMetersPerSecond *
                          rules.designSpeedMetersPerSecond *
                          value.curvaturePerMeter /
                          rules.nominalGravityMetersPerSecondSquared),
                -rules.maximumBankRadians, rules.maximumBankRadians);
            if (!actualBankRadians.empty())
            {
                if (std::abs(actualBankRadians[i]) > rules.maximumBankRadians)
                    warn(CorridorIssue::ExcessiveBank, s.stationMeters,
                         std::abs(actualBankRadians[i]), rules.maximumBankRadians,
                         "Authored road bank / railway cant exceeds the configured maximum.");
                const f64 gradient = std::abs(
                    actualBankRadians[i] - actualBankRadians[i-1]) / distance;
                if (gradient > rules.maximumBankGradientRadiansPerMeter)
                    warn(CorridorIssue::AbruptBankTransition, s.stationMeters,
                         gradient, rules.maximumBankGradientRadiansPerMeter,
                         "Authored banking / cant transition is too abrupt.");
            }
        }
        out.stations.push_back(value);
    }
    return out;
}
std::vector<f64> SuggestCorridorBankProfile(
    const CorridorEngineeringReport& report,
    const CorridorEngineeringRules& rules)
{
    if (!report.Valid() || !std::isfinite(rules.maximumBankRadians) ||
        rules.maximumBankRadians < 0.0 ||
        !std::isfinite(rules.maximumBankGradientRadiansPerMeter) ||
        rules.maximumBankGradientRadiansPerMeter < 0.0)
        return {};
    std::vector<f64> bank;
    bank.reserve(report.stations.size());
    for (const auto& s : report.stations)
    {
        if (!std::isfinite(s.stationMeters) ||
            !std::isfinite(s.recommendedBankRadians) ||
            (!bank.empty() && s.stationMeters <=
                report.stations[bank.size()-1U].stationMeters))
            return {};
        bank.push_back(std::clamp(s.recommendedBankRadians,
            -rules.maximumBankRadians, rules.maximumBankRadians));
    }
    // Bidirectional rate limiting reduces abrupt cant transitions without
    // altering any stored geometry. This is an advisory approximation, not
    // a validated rail transition-curve solver.
    for (std::size_t i = 1; i < bank.size(); ++i)
    {
        const f64 limit = rules.maximumBankGradientRadiansPerMeter *
            (report.stations[i].stationMeters - report.stations[i-1].stationMeters);
        bank[i] = std::clamp(bank[i], bank[i-1]-limit, bank[i-1]+limit);
    }
    for (std::size_t i = bank.size(); i > 1U; --i)
    {
        const std::size_t previous = i-2U;
        const f64 limit = rules.maximumBankGradientRadiansPerMeter *
            (report.stations[previous+1U].stationMeters -
             report.stations[previous].stationMeters);
        bank[previous] = std::clamp(bank[previous],
            bank[previous+1U]-limit, bank[previous+1U]+limit);
    }
    return bank;
}
} // namespace orbit::path_geometry
