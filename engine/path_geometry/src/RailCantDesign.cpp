#include <orbit/path_geometry/RailCantDesign.hpp>
#include <algorithm>
#include <cmath>

namespace orbit::path_geometry
{
bool RailCantPlan::Valid() const noexcept
{
    for (const auto& f:findings)
        if (f.issue==RailCantIssue::InvalidInput) return false;
    return true;
}
RailCantPlan DesignRailCant(const CorridorEngineeringReport& alignment,
                            const RailCantLimits& limits)
{
    RailCantPlan out;
    const auto invalid=[&]() {
        out.findings.push_back({RailCantIssue::InvalidInput,0,0,0});
        return out;
    };
    const auto ok=[](f64 value) {return std::isfinite(value);};
    if (!alignment.Valid() || !ok(limits.gaugeMeters) ||
        limits.gaugeMeters<=0 ||
        !ok(limits.designSpeedMetersPerSecond) ||
        !ok(limits.minimumOperatingSpeedMetersPerSecond) ||
        !ok(limits.maximumOperatingSpeedMetersPerSecond) ||
        limits.minimumOperatingSpeedMetersPerSecond<0 ||
        limits.minimumOperatingSpeedMetersPerSecond>limits.designSpeedMetersPerSecond ||
        limits.designSpeedMetersPerSecond>limits.maximumOperatingSpeedMetersPerSecond ||
        !ok(limits.maximumCantMeters) || limits.maximumCantMeters<0 ||
        limits.maximumCantMeters>=limits.gaugeMeters ||
        !ok(limits.maximumCantDeficiencyMeters) ||
        limits.maximumCantDeficiencyMeters<0 ||
        !ok(limits.maximumCantExcessMeters) ||
        limits.maximumCantExcessMeters<0 ||
        !ok(limits.maximumCantGradient) || limits.maximumCantGradient<0 ||
        !ok(limits.gravityMetersPerSecondSquared) ||
        limits.gravityMetersPerSecondSquared<=0)
        return invalid();
    const auto cant=[&](f64 curvature,f64 speed) {
        return limits.gaugeMeters*
            std::sin(std::atan(curvature*speed*speed/
                limits.gravityMetersPerSecondSquared));
    };
    for (std::size_t i=0;i<alignment.stations.size();++i)
    {
        const auto& s=alignment.stations[i];
        if (!ok(s.stationMeters) || !ok(s.curvaturePerMeter) ||
            (i && s.stationMeters<=alignment.stations[i-1].stationMeters))
            return invalid();
        const f64 slow=cant(s.curvaturePerMeter,
            limits.minimumOperatingSpeedMetersPerSecond);
        const f64 fast=cant(s.curvaturePerMeter,
            limits.maximumOperatingSpeedMetersPerSecond);
        const f64 ideal=cant(s.curvaturePerMeter,
            limits.designSpeedMetersPerSecond);
        const f64 minimum=std::max(0.0,std::abs(fast)-
            limits.maximumCantDeficiencyMeters);
        const f64 maximum=std::min(limits.maximumCantMeters,
            std::abs(slow)+limits.maximumCantExcessMeters);
        if (minimum>maximum+1.e-12)
            out.findings.push_back({RailCantIssue::IncompatibleSpeeds,
                s.stationMeters,minimum,maximum});
        const f64 target=std::clamp(std::abs(ideal),0.0,
            std::max(0.0,maximum));
        const f64 wanted=std::max(minimum,target);
        const f64 signedCant=std::copysign(
            std::min(wanted,limits.maximumCantMeters),
            s.curvaturePerMeter);
        out.stations.push_back({.stationMeters=s.stationMeters,
            .equilibriumAtMinimumSpeedMeters=slow,
            .equilibriumAtMaximumSpeedMeters=fast,
            .suggestedCantMeters=signedCant});
    }
    // Two-sided linear runout bound, including S-curves and cant reversals.
    // A constrained result is never silently treated as sufficient: report
    // any remaining fast/slow imbalance below.
    for (std::size_t i=1;i<out.stations.size();++i)
    {
        const f64 change=limits.maximumCantGradient*
            (out.stations[i].stationMeters-out.stations[i-1].stationMeters);
        auto& h=out.stations[i].suggestedCantMeters;
        const f64 p=out.stations[i-1].suggestedCantMeters;
        h=std::clamp(h,p-change,p+change);
    }
    for (std::size_t i=out.stations.size();i>1;--i)
    {
        const std::size_t j=i-2;
        const f64 change=limits.maximumCantGradient*
            (out.stations[j+1].stationMeters-out.stations[j].stationMeters);
        auto& h=out.stations[j].suggestedCantMeters;
        const f64 p=out.stations[j+1].suggestedCantMeters;
        h=std::clamp(h,p-change,p+change);
    }
    for (auto& s:out.stations)
    {
        s.suggestedBankRadians=std::asin(std::clamp(
            s.suggestedCantMeters/limits.gaugeMeters,-1.0,1.0));
        const f64 sign=s.equilibriumAtMaximumSpeedMeters<0 ? -1.0 : 1.0;
        const f64 actual=sign*s.suggestedCantMeters;
        const f64 fast=sign*s.equilibriumAtMaximumSpeedMeters;
        const f64 slow=sign*s.equilibriumAtMinimumSpeedMeters;
        s.remainingDeficiencyMeters=std::max(0.0,fast-actual);
        s.remainingExcessMeters=std::max(0.0,actual-slow);
        if (s.remainingDeficiencyMeters>
            limits.maximumCantDeficiencyMeters+1.e-9)
            out.findings.push_back({RailCantIssue::ExcessCantDeficiency,
                s.stationMeters,s.remainingDeficiencyMeters,
                limits.maximumCantDeficiencyMeters});
        if (s.remainingExcessMeters>limits.maximumCantExcessMeters+1.e-9)
            out.findings.push_back({RailCantIssue::ExcessCantAtSlowSpeed,
                s.stationMeters,s.remainingExcessMeters,
                limits.maximumCantExcessMeters});
    }
    return out;
}
} // namespace orbit::path_geometry
