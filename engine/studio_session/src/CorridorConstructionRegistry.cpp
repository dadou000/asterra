#include <orbit/studio_session/CorridorConstructionRegistry.hpp>
#include <orbit/path_geometry/CorridorSweep.hpp>
#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace orbit::studio_session
{
std::vector<terrain::PhysicalTerrainPageAddress>
CorridorConstructionRegistry::PagesForBody(
    const universe::BodyId body, const std::size_t maximumPages,
    const math::Double3* observerDirection,
    const std::span<const terrain::PhysicalTerrainPageAddress> previous) const
{
    std::vector<terrain::PhysicalTerrainPageAddress> pages;
    if (maximumPages == 0U) return pages;
    const auto found = records_.find(body);
    if (found == records_.end()) return pages;
    for (const auto& [id, entry] : found->second.entries)
    {
        (void)id;
        pages.insert(pages.end(), entry.pages.begin(), entry.pages.end());
    }
    std::sort(pages.begin(), pages.end(), [](const auto& a, const auto& b) {
        return std::tuple{a.planet.high, a.planet.low,
            static_cast<u8>(a.tile.face), a.tile.level, a.tile.x, a.tile.y} <
            std::tuple{b.planet.high, b.planet.low,
            static_cast<u8>(b.tile.face), b.tile.level, b.tile.x, b.tile.y};
    });
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    if (observerDirection && math::LengthSquared(*observerDirection) > 0.5)
    {
        const auto direction = math::Normalize(*observerDirection);
        std::stable_sort(pages.begin(), pages.end(),
            [&](const auto& a, const auto& b) {
                const f64 da = math::Dot(direction,
                    world::CubeToUnitDirection(world::TileCenter(a.tile)));
                const f64 db = math::Dot(direction,
                    world::CubeToUnitDirection(world::TileCenter(b.tile)));
                return da > db;
            });
    }
    // Existing resident pages retain a small angular preference, so tiny
    // observer motion does not continuously replace the final budget slot.
    // The bias is bounded: a substantially closer page still takes priority.
    if (observerDirection && math::LengthSquared(*observerDirection) > 0.5 &&
        !previous.empty())
    {
        const auto direction = math::Normalize(*observerDirection);
        const auto score = [&](const auto& page) {
            const f64 proximity = math::Dot(direction,
                world::CubeToUnitDirection(world::TileCenter(page.tile)));
            const bool resident = std::find(previous.begin(), previous.end(),
                page) != previous.end();
            return proximity + (resident ? 0.002 : 0.0);
        };
        std::stable_sort(pages.begin(), pages.end(),
            [&](const auto& a, const auto& b) { return score(a) > score(b); });
    }
    if (pages.size() > maximumPages) pages.resize(maximumPages);
    return pages;
}

bool CorridorConstructionRegistry::Publish(
    const universe::BodyId body,
    const world::PlanetDefinition planet,
    const terrain::TerrainSource& base,
    const u8 physicalPageLevel,
    std::vector<CorridorConstructionEntry> entries,
    StudioTerrainPhysicalPageService& physical,
    const std::size_t maximumPages)
{
    if (!body || !planet.id || maximumPages == 0 ||
        physicalPageLevel > 24)
        throw std::invalid_argument("Invalid construction publication target.");

    std::sort(entries.begin(), entries.end(),
        [](const auto& a, const auto& b) { return a.layer.edge < b.layer.edge; });
    for (std::size_t i=0; i<entries.size(); ++i)
        if (!entries[i].layer.edge || entries[i].revision == 0 ||
            (i && entries[i-1].layer.edge == entries[i].layer.edge))
            throw std::invalid_argument("Invalid or duplicate construction entry.");

    const auto old = records_.find(body);
    if (old != records_.end() && old->second.planet != planet.id)
        throw std::invalid_argument("Planet changed without clearing construction registry.");

    // Fast path before any planetary projection. Editing unrelated scene
    // objects must not walk every road's stations on every Studio tick.
    if (old != records_.end() &&
        old->second.level == physicalPageLevel &&
        old->second.sourceRevision == base.Revision() &&
        old->second.entries.size() == entries.size())
    {
        bool revisionsMatch = true;
        for (const auto& entry : entries)
        {
            const auto previous = old->second.entries.find(entry.layer.edge);
            if (previous == old->second.entries.end() ||
                previous->second.revision != entry.revision)
            {
                revisionsMatch = false;
                break;
            }
        }
        if (revisionsMatch) return false;
    }

    BodyState next{};
    next.planet = planet.id;
    next.level = physicalPageLevel;
    next.sourceRevision = base.Revision();
    next.publicationRevision =
        old == records_.end() ? 1U : old->second.publicationRevision + 1U;

    std::vector<path_geometry::CorridorTerrainLayer> layers;
    std::vector<terrain::PhysicalTerrainPageAddress> changedPages;
    bool changed = old == records_.end() ? !entries.empty() :
        old->second.level != physicalPageLevel ||
        old->second.sourceRevision != next.sourceRevision;
    for (auto& entry : entries)
    {
        std::vector<math::Double3> points;
        points.reserve(entry.layer.stations.size());
        for (const auto& station : entry.layer.stations)
            points.push_back(station.position);
        const f64 radius = entry.layer.footprint.halfWidthMeters +
            entry.layer.footprint.shoulderMeters +
            entry.layer.footprint.blendMeters;
        const auto projected = path_geometry::ProjectCorridorToTerrainPages(
            planet, points, radius, physicalPageLevel, maximumPages);
        if (!projected || !path_geometry::CorridorFootprintEnvelope(
            entry.layer.stations, entry.layer.footprint))
            throw std::invalid_argument("Construction footprint projection failed.");

        const auto previous = old == records_.end() ? old :
            records_.find(body);
        const Record* prior = nullptr;
        if (previous != records_.end())
        {
            const auto match = previous->second.entries.find(entry.layer.edge);
            if (match != previous->second.entries.end())
                prior = &match->second;
        }
        const bool modified = !prior || prior->revision != entry.revision ||
            old->second.level != physicalPageLevel ||
            old->second.sourceRevision != next.sourceRevision ||
            prior->pages != *projected;
        if (modified)
        {
            changed = true;
            if (prior) changedPages.insert(changedPages.end(),
                prior->pages.begin(), prior->pages.end());
            changedPages.insert(changedPages.end(),
                projected->begin(), projected->end());
        }
        next.entries.emplace(entry.layer.edge,
            Record{entry.revision, std::move(*projected)});
        layers.push_back(std::move(entry.layer));
    }
    if (old != records_.end())
        for (const auto& [id, previous] : old->second.entries)
            if (!next.entries.contains(id))
            {
                changed = true;
                changedPages.insert(changedPages.end(),
                    previous.pages.begin(), previous.pages.end());
            }

    if (!changed) return false;
    // No construction footprint means there is nothing to republish or
    // invalidate, even if the upstream analytic source was recomposed.
    // Retain updated source identity to avoid retrying every tick.
    if (changedPages.empty())
    {
        if (next.entries.empty())
        {
            records_.insert_or_assign(body, std::move(next));
            return false;
        }
        throw std::logic_error(
            "Changed corridor construction has no affected terrain pages.");
    }

    std::sort(changedPages.begin(), changedPages.end(),
        [](const auto& a, const auto& b) {
            return std::tuple{a.planet.high, a.planet.low,
                static_cast<u8>(a.tile.face), a.tile.level, a.tile.x, a.tile.y} <
                std::tuple{b.planet.high, b.planet.low,
                static_cast<u8>(b.tile.face), b.tile.level, b.tile.x, b.tile.y};
        });
    changedPages.erase(std::unique(changedPages.begin(), changedPages.end()),
        changedPages.end());
    if (changedPages.size() > maximumPages)
        throw std::invalid_argument("Combined old/new footprint exceeds page budget.");
    auto overlay = std::make_shared<path_geometry::CorridorTerrainSource>(
        base, planet, std::move(layers), next.publicationRevision);
    physical.StageCorridorConstruction(body, overlay, changedPages);
    records_.insert_or_assign(body, std::move(next));
    return true;
}
} // namespace orbit::studio_session
