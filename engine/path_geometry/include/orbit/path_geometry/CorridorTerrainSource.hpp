#pragma once

#include <orbit/path_geometry/CorridorSweep.hpp>
#include <orbit/terrain/TerrainSource.hpp>

#include <memory>
#include <vector>

namespace orbit::path_geometry
{
// Immutable, body-centred construction record. Positions and stations are
// expressed in metres relative to the planet centre, while station up vectors
// describe radial elevation. This layer does not own authored terrain data.
struct CorridorTerrainLayer
{
    scene::ObjectId edge{};
    std::vector<PathStation> stations;
    CorridorTerrainFootprint footprint{};
};

// An immutable terrain-source decorator, safe for concurrent page workers.
// Upstream source must outlive the decorator. Replacing a layer set constructs
// another source, so workers cannot observe partially edited corridors.
class CorridorTerrainSource final : public terrain::TerrainSource
{
public:
    CorridorTerrainSource(
        const terrain::TerrainSource& source,
        world::PlanetDefinition planet,
        std::vector<CorridorTerrainLayer> layers,
        u64 authoringRevision);

    [[nodiscard]] terrain::TerrainSample Sample(
        const terrain::TerrainQuery& query) const noexcept override;

    [[nodiscard]] u64 Revision() const noexcept override;
    [[nodiscard]] terrain::TerrainGenerationRevisions
    GenerationRevisions() const noexcept override;

private:
    const terrain::TerrainSource* source_;
    world::PlanetDefinition planet_;
    std::vector<CorridorTerrainLayer> layers_;
    u64 authoringRevision_{0};
};
} // namespace orbit::path_geometry
