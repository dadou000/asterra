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
        std::shared_ptr<const terrain::TerrainSource> source,
        world::PlanetDefinition planet,
        std::vector<CorridorTerrainLayer> layers,
        u64 authoringRevision);

    CorridorTerrainSource(
        const terrain::TerrainSource& source,
        world::PlanetDefinition planet,
        std::vector<CorridorTerrainLayer> layers,
        u64 authoringRevision);

    // Create a new immutable overlay bound to the lifetime of a replacement
    // terrain source while preserving corridor geometry and revisions.
    [[nodiscard]] std::shared_ptr<const CorridorTerrainSource> Rebind(
        std::shared_ptr<const terrain::TerrainSource> source) const;

    [[nodiscard]] terrain::TerrainSample Sample(
        const terrain::TerrainQuery& query) const noexcept override;

    [[nodiscard]] u64 Revision() const noexcept override;
    [[nodiscard]] terrain::TerrainGenerationRevisions
    GenerationRevisions() const noexcept override;

private:
    std::shared_ptr<const terrain::TerrainSource> ownedSource_;
    const terrain::TerrainSource* source_;

    world::PlanetDefinition planet_;
    std::vector<CorridorTerrainLayer> layers_;
    u64 authoringRevision_{0};
};
} // namespace orbit::path_geometry
