#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/terrain_dependency/TerrainDependencyGraph.hpp>
#include <orbit/world/Planet.hpp>

#include <span>
#include <vector>

namespace orbit::studio_session
{
// Converts one M09 brush/spline footprint into bounded M27 invalidations.
// Long splines are sampled into multiple local scopes; no returned request is
// global and every scope respects M27's <=64-tile combined reach.
[[nodiscard]] std::vector<
    terrain_dependency::TerrainInvalidationRequest>
BuildTerrainAuthoringInvalidations(
    const world::PlanetDefinition& planet,
    std::span<const math::Double3> controlUnitDirections,
    f64 influenceRadiusMeters,
    u8 physicalTileLevel,
    u32 downstreamRadiusTiles = 2U);
} // namespace orbit::studio_session
