#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_region/DerivedTerrainRegionCache.hpp>
#include <orbit/world/Planet.hpp>

#include <memory>

namespace orbit::terrain_region
{
struct DerivedRegionTerrainSourceConfig
{
    u32 neighborhoodRadius{1};

    // Relative to the regional hydrology sample spacing.
    f64 fullDetailFootprintScale{0.5};
    f64 fadeOutFootprintScale{4.0};

    f32 maximumWetlandBlend{0.55F};
};

class DerivedRegionTerrainSource final :
    public terrain::TerrainSource
{
public:
    DerivedRegionTerrainSource(
        world::PlanetDefinition planet,
        std::shared_ptr<const terrain::TerrainSource> source,
        std::shared_ptr<DerivedTerrainRegionCache> regionCache,
        DerivedRegionTerrainSourceConfig config = {});

    [[nodiscard]] terrain::TerrainSample Sample(
        const terrain::TerrainQuery& query) const noexcept override;

    [[nodiscard]] u64 Revision() const noexcept override;

private:
    world::PlanetDefinition planet_{};

    std::shared_ptr<const terrain::TerrainSource>
        source_;

    std::shared_ptr<DerivedTerrainRegionCache>
        regionCache_;

    DerivedRegionTerrainSourceConfig config_{};
};
} // namespace orbit::terrain_region
