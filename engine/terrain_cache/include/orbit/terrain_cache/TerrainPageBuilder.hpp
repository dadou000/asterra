#pragma once

#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_cache/TerrainPage.hpp>
#include <orbit/world/Planet.hpp>

namespace orbit::terrain_cache
{
[[nodiscard]] TerrainPage BuildTerrainPage(
    const world::PlanetDefinition& planet,
    const terrain::TerrainSource& source,
    const TerrainPageDesc& desc);
} // namespace orbit::terrain_cache
