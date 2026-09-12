#pragma once

#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_erosion/RiverCarving.hpp>
#include <orbit/world/Planet.hpp>

#include <memory>
#include <vector>

namespace orbit::terrain_erosion
{
struct RiverCarvedTerrainConfig
{
    // Relative to full valley diameter.
    f64 fullDetailFootprintRatio{0.20};
    f64 fadeOutFootprintRatio{1.25};

    // Adds a wetland tendency near active channels for semantic/material use.
    f32 maximumWetlandBlend{0.55F};
};

class RiverCarvedTerrainSource final :
    public terrain::TerrainSource
{
public:
    RiverCarvedTerrainSource(
        world::PlanetDefinition planet,
        std::shared_ptr<const terrain::TerrainSource> source,
        std::vector<RiverCarvingField> fields,
        RiverCarvedTerrainConfig config = {});

    [[nodiscard]] terrain::TerrainSample Sample(
        const terrain::TerrainQuery& query) const noexcept override;

    [[nodiscard]] u64 Revision() const noexcept override;

    [[nodiscard]] const std::vector<RiverCarvingField>&
    Fields() const noexcept;

private:
    world::PlanetDefinition planet_{};
    std::shared_ptr<const terrain::TerrainSource>
        source_;

    std::vector<RiverCarvingField> fields_;
    RiverCarvedTerrainConfig config_{};
    u64 localRevision_{0};
};
} // namespace orbit::terrain_erosion
