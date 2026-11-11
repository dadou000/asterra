#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/terrain_dependency/TerrainDependencyGraph.hpp>
#include <orbit/terrain_impacts/ImpactField.hpp>
#include <orbit/world/Planet.hpp>

#include <span>
#include <string_view>
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
    u32 downstreamRadiusTiles = 2U,
    terrain_dependency::TerrainChangeKind kind =
        terrain_dependency::TerrainChangeKind::TerrainAuthoring);

// Diffs two persisted impact-history recipes. Local authored crater and flow
// edits invalidate only their conservative influence bounds; procedural
// populations and global age/environment/fracture changes invalidate the
// whole planet.
[[nodiscard]] std::vector<terrain_dependency::TerrainInvalidationRequest>
BuildImpactHistoryInvalidations(
    const world::PlanetDefinition& planet,
    std::string_view previousToml,
    std::string_view nextToml,
    u8 physicalTileLevel,
    u32 downstreamRadiusTiles = 3U);
} // namespace orbit::studio_session
