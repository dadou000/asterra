#pragma once

#include <orbit/fields/FieldRegistry.hpp>
#include <orbit/path_routing/RoutePlanner.hpp>
#include <orbit/surface/SurfaceRegistry.hpp>

#include <string_view>
#include <vector>

namespace orbit::path_routing
{
// Terrain-backed surface domain for the current spherical terrain capability.
// The projector snaps every search candidate to terrain elevation and exports
// water depth for profile bridge/water costs. Terrain Revision() participates
// in selective invalidation.
[[nodiscard]] RouteEnvironment
MakeTerrainSurfaceEnvironment(
    universe::BodyId body,
    frames::FrameId routeFrame,
    time::SimulationTime atTime,
    const frames::FrameGraph& frames,
    const universe::BodyRegistry& bodies,
    const surface::SurfaceRegistry& surfaces,
    RouteSearchConfig search = {});

// CPU/hybrid scalar fields can add arbitrary buildability/avoidance cost.
// Returning no scalar sample makes that candidate impassable.
[[nodiscard]] RouteCostSource
MakeSurfaceScalarFieldCostSource(
    std::string key,
    fields::FieldId field,
    f64 weight,
    universe::BodyId body,
    frames::FrameId routeFrame,
    time::SimulationTime atTime,
    const frames::FrameGraph& frames,
    const universe::BodyRegistry& bodies,
    const fields::FieldRegistry& fields);

// Resolves PathProfile::preferredCostFields by descriptor name for the body.
// Unknown/GPU-only/non-scalar fields are skipped rather than faked.
[[nodiscard]] std::vector<RouteCostSource>
MakePreferredSurfaceFieldCosts(
    const paths::PathProfile& profile,
    f64 weight,
    universe::BodyId body,
    frames::FrameId routeFrame,
    time::SimulationTime atTime,
    const frames::FrameGraph& frames,
    const universe::BodyRegistry& bodies,
    const fields::FieldRegistry& fields);
} // namespace orbit::path_routing
