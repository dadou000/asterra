#pragma once
#include <orbit/path_geometry/TransportSections.hpp>
#include <orbit/path_geometry/RailCantDesign.hpp>

namespace orbit::path_geometry
{
// A disposable, read-only design product. It carries no path ownership and
// never mutates a PathNetworkService record.
struct InfrastructureSectionPlan
{
    CorridorSweep section;
    CorridorEngineeringReport engineering;
    RailCantPlan railwayCant;
};
// Uses the SAME centreline/profiles/build options as BuildCorridorDerived.
// "applyAutomaticCant" only affects the derived mesh, never authored intent.
[[nodiscard]] InfrastructureSectionPlan PlanInfrastructureSection(
    const PathCenterline& centerline,
    const paths::PathProfile& profile,
    PathBuildOptions options = {},
    f64 designSpeedMetersPerSecond = 0.0,
    bool applyAutomaticCant = true);
} // namespace orbit::path_geometry
