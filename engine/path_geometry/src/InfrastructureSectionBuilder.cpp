#include <orbit/path_geometry/InfrastructureSectionBuilder.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::path_geometry
{
InfrastructureSectionPlan PlanInfrastructureSection(
    const PathCenterline& centerline,
    const paths::PathProfile& profile,
    const PathBuildOptions options,
    const f64 designSpeedMetersPerSecond,
    const bool applyAutomaticCant)
{
    if ((profile.kind != paths::PathProfileKind::Road &&
         profile.kind != paths::PathProfileKind::Rail) ||
        profile.lanes == 0U || profile.lanes > 32U ||
        !std::isfinite(designSpeedMetersPerSecond) ||
        designSpeedMetersPerSecond < 0.0)
        throw std::invalid_argument(
            "Transport section requires road/rail profile, positive lane/track count, and valid speed.");

    // Exact same source stations are used to determine cant and later sweep
    // refinement. There is no independent second curve interpreter.
    const auto base = BuildPathDerived(centerline, profile, options);
    CorridorEngineeringRules engineering =
        EngineeringRulesForProfile(profile);
    const f64 speed = designSpeedMetersPerSecond > 0.0 ?
        designSpeedMetersPerSecond :
        (profile.kind == paths::PathProfileKind::Rail ? 22.0 : 20.0);
    engineering.designSpeedMetersPerSecond = speed;
    InfrastructureSectionPlan out;
    out.engineering = AnalyzeCorridorEngineering(base.stations,engineering);
    if (!out.engineering.Valid())
        throw std::invalid_argument("Invalid transport engineering stations.");
    if (profile.kind == paths::PathProfileKind::Road)
    {
        RoadSectionDesign road;
        road.laneCount = profile.lanes;
        road.laneWidthMeters = profile.widthMeters/
            static_cast<f64>(profile.lanes);
        out.section = MakeRoadSweep(road);
        return out;
    }

    RailSectionDesign rail;
    rail.trackCount = profile.lanes;
    // A rail path profile's width is its entire formation width. Gauge,
    // track centres, railhead widths and sleepers remain independently
    // configurable in RailSectionDesign; profile integration here uses
    // defaults until the dedicated railway profile asset is extended.
    const f64 trackEnvelope = rail.gaugeMeters +
        (static_cast<f64>(rail.trackCount)-1.0)*rail.trackSpacingMeters;
    if (profile.widthMeters <= trackEnvelope + 0.2)
        throw std::invalid_argument(
            "Rail formation width cannot contain gauge and requested tracks.");
    rail.formationMarginMeters = (profile.widthMeters-trackEnvelope)*0.5;
    if (!applyAutomaticCant)
    {
        out.section = MakeRailSweep(rail);
        return out;
    }

    RailCantLimits limits;
    limits.gaugeMeters = rail.gaugeMeters;
    limits.designSpeedMetersPerSecond = speed;
    limits.minimumOperatingSpeedMetersPerSecond = speed*0.5;
    limits.maximumOperatingSpeedMetersPerSecond = speed;
    out.railwayCant = DesignRailCant(out.engineering,limits);
    if (!out.railwayCant.Valid())
        throw std::invalid_argument("Rail cant constraints have invalid input.");
    std::vector<f64> angles;
    angles.reserve(out.railwayCant.stations.size());
    for (const auto& station : out.railwayCant.stations)
        angles.push_back(station.suggestedBankRadians);
    out.section = MakeRailSweep(rail,std::move(angles));
    return out;
}
} // namespace orbit::path_geometry
