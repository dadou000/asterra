#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain/BakedRivers.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain_erosion/RiverNetwork.hpp>
#include <orbit/terrain_hydrology/DrainagePage.hpp>

#include <span>

namespace orbit::studio_session
{
// Raises drainage area and discharge along the baked river channels so the
// page's own flow solve agrees with the basin-scale discharge stored in the
// bake. Only cells inside the bankfull channel are touched.
void ApplyBakedRiverDischarge(
    terrain_hydrology::DrainagePage& drainage,
    const terrain::BakedRiverNetwork& baked,
    std::span<const terrain_hydrology::DrainageCellInput> inputs);

// Clips the baked river graph to one physical page and returns it as the
// page's river network. Nothing is routed or evolved: nodes sit on the stored
// centerlines, widths/depths/discharge come from the bake, and the water level
// follows the channel bed already carved into the terrain.
[[nodiscard]] terrain_erosion::RiverNetwork BuildBakedPageRiverNetwork(
    const terrain::BakedRiverNetwork& baked,
    const terrain_hydrology::DrainagePage& drainage,
    f64 maximumNodeSpacingMeters);
} // namespace orbit::studio_session
