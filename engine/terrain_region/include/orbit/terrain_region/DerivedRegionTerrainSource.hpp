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
    // Relative to the regional hydrology sample spacing.
    f64 fullDetailFootprintScale{0.5};
    f64 fadeOutFootprintScale{4.0};

    f32 maximumWetlandBlend{0.55F};
};

class DerivedRegionTerrainSource final :
    public terrain::TerrainSource
{
public:
    // `fineRegionCache`, when provided, is a second region cache at a
    // finer tile level (smaller physical tiles, same grid
    // resolution -- see DerivedTerrainRegionConfig::hydrology --
    // hence proportionally finer real-world sample spacing). Wherever
    // it has ready coverage, it fully replaces `regionCache`'s
    // contribution instead of blending with it, so the near-camera
    // ground shape and the coarse-region ground shape don't average
    // into something that matches neither. Outside its coverage
    // (typically just a streamed neighborhood around the observer),
    // sampling falls back to `regionCache` exactly as before.
    DerivedRegionTerrainSource(
        world::PlanetDefinition planet,
        std::shared_ptr<const terrain::TerrainSource> source,
        std::shared_ptr<DerivedTerrainRegionCache> regionCache,
        std::shared_ptr<DerivedTerrainRegionCache> fineRegionCache,
        DerivedRegionTerrainSourceConfig config = {});

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

    std::shared_ptr<DerivedTerrainRegionCache>
        fineRegionCache_;

    DerivedRegionTerrainSourceConfig config_{};

    [[nodiscard]] bool AccumulateFromCache(
        const DerivedTerrainRegionCache& cache,
        const math::Double3& direction,
        const terrain::TerrainQuery& query,
        f64 baseElevation,
        f64& totalWeight,
        f64& weightedRegionalDelta,
        f64& weightedCarveDelta,
        f64& weightedWetlandInfluence)
        const noexcept;
};
} // namespace orbit::terrain_region
