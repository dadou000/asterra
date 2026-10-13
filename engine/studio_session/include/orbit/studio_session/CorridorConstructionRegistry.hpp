#pragma once
#include <orbit/path_geometry/CorridorTerrainSource.hpp>
#include <orbit/studio_session/StudioTerrainPhysicalPageService.hpp>
#include <orbit/universe/BodyRegistry.hpp>
#include <map>
#include <memory>
#include <vector>

namespace orbit::studio_session
{
// Complete immutable set for one body. Callers must resolve body-fixed frames
// before submission; no camera or inertial-frame guesses are permitted.
struct CorridorConstructionEntry
{
    path_geometry::CorridorTerrainLayer layer;
    u64 revision{0};
};

class CorridorConstructionRegistry
{
public:
    // Reconcile an entire body's corridor set, including deletions.
    // Returns false for an identical snapshot. Publication is transactional:
    // previous footprints remain tracked if mapping/staging fails.
    [[nodiscard]] bool Publish(
        universe::BodyId body,
        world::PlanetDefinition planet,
        const terrain::TerrainSource& base,
        u8 physicalPageLevel,
        std::vector<CorridorConstructionEntry> entries,
        StudioTerrainPhysicalPageService& physical,
        std::size_t maximumPages = 4096);

    void Clear() noexcept { records_.clear(); }

private:
    struct Record
    {
        u64 revision{0};
        std::vector<terrain::PhysicalTerrainPageAddress> pages;
    };
    struct BodyState
    {
        world::PlanetId planet{};
        u8 level{0};
        std::map<scene::ObjectId, Record> entries;
        u64 publicationRevision{0};
    };
    std::map<universe::BodyId, BodyState> records_;
};
} // namespace orbit::studio_session
