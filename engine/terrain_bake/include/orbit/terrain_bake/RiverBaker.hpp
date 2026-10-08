#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain/BakedRivers.hpp>
#include <orbit/terrain_bake/TectonicBaker.hpp>
#include <orbit/world/Planet.hpp>

#include <memory>

namespace orbit::terrain_bake
{
struct RiverBakeOptions
{
    // Hydrology grid cells per cube-face edge. Cell size is the finest river
    // the graph can resolve; narrower channels come from the baked width.
    u32 resolution{256};
    // Only rivers carrying at least this discharge become part of the graph.
    f64 minimumDischargeCubicMetersPerSecond{1'000.0};
    // Annual runoff depth for a precipitation value of 1.
    f64 annualRunoffMeters{0.5};
    u32 workerThreads{0};

    [[nodiscard]] bool IsValid() const noexcept
    {
        return resolution >= 16U && resolution <= 1024U &&
               minimumDischargeCubicMetersPerSecond > 0.0 &&
               annualRunoffMeters >= 0.0;
    }
};

// Identity of everything a river bake depends on: the terrain recipe it is
// computed on (including the installed tectonic bake), the grid and the runoff
// parameters, and the baker's algorithm version.
[[nodiscard]] u64 RiverBakeRecipeHash(
    const world::PlanetDefinition& planet,
    const terrain::AnalyticTerrainDesc& desc,
    const RiverBakeOptions& options);

// Bakes the planet's river network once: samples the terrain onto a cube grid,
// fills depressions toward the ocean, accumulates discharge from precipitation
// and extracts the major rivers as a graph. `desc` should carry the tectonic
// bake the terrain will use; any river network on it is ignored. Returns an
// empty network for a planet without an ocean, nullptr when cancelled, and
// throws std::invalid_argument for invalid options.
[[nodiscard]] std::shared_ptr<const terrain::BakedRiverNetwork> BakeRivers(
    const world::PlanetDefinition& planet,
    terrain::AnalyticTerrainDesc desc,
    const RiverBakeOptions& options,
    BakeControl* control = nullptr);
} // namespace orbit::terrain_bake
