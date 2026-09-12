#include <orbit/terrain_region/DerivedTerrainRegionStreamer.hpp>

#include <orbit/math/Vector.hpp>
#include <orbit/world/PlanetTileNeighborhood.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace orbit::terrain_region
{
namespace
{
void AddUnique(
    std::vector<world::PlanetTileId>& tiles,
    const world::PlanetTileId tile)
{
    if (std::find(
            tiles.begin(),
            tiles.end(),
            tile) ==
        tiles.end())
    {
        tiles.push_back(tile);
    }
}
} // namespace

DerivedTerrainRegionStreamer::
DerivedTerrainRegionStreamer(
    const world::PlanetDefinition planet,
    DerivedTerrainRegionCache& cache,
    const DerivedTerrainRegionStreamerConfig config)
    : planet_(planet),
      cache_(cache),
      config_(config)
{
    if (planet_.radiusMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Orbit derived terrain region streamer requires a positive planet radius.");
    }

    if (config_.forwardPrefetchDistanceTiles <
        0.0)
    {
        throw std::invalid_argument(
            "Orbit derived terrain region streamer prefetch distance cannot be negative.");
    }
}

void DerivedTerrainRegionStreamer::Update(
    const math::Double3& observerDirection,
    const math::Double3& surfaceTravelDirection)
{
    const math::Double3 observer =
        math::Normalize(
            observerDirection);

    if (math::LengthSquared(observer) <=
        0.0)
    {
        return;
    }

    const DerivedTerrainRegionId currentId =
        cache_.IdForDirection(observer);

    std::vector<world::PlanetTileId> desired;
    desired.reserve(
        static_cast<std::size_t>(
            config_.neighborhoodRadius * 2U + 1U) *
        static_cast<std::size_t>(
            config_.neighborhoodRadius * 2U + 1U) +
        1U);

    AddUnique(
        desired,
        currentId.tile);

    for (const world::PlanetTileId tile :
         world::TileNeighborhood(
             currentId.tile,
             config_.neighborhoodRadius))
    {
        AddUnique(
            desired,
            tile);
    }

    world::PlanetTileId prefetchTile =
        currentId.tile;

    bool hasPrefetchTile = false;

    math::Double3 tangent =
        surfaceTravelDirection -
        observer *
            math::Dot(
                surfaceTravelDirection,
                observer);

    if (config_.forwardPrefetchDistanceTiles >
            0.0 &&
        math::LengthSquared(tangent) >
            1.0e-12)
    {
        tangent =
            math::Normalize(tangent);

        const f64 tileWidth =
            world::ApproximateTileWidthMeters(
                planet_,
                currentId.tile);

        const f64 angle =
            tileWidth /
            planet_.radiusMeters *
            config_.
                forwardPrefetchDistanceTiles;

        const math::Double3 predictedDirection =
            math::Normalize(
                observer *
                    std::cos(angle) +
                tangent *
                    std::sin(angle));

        prefetchTile =
            world::TileForDirection(
                predictedDirection,
                cache_.TileLevel());

        if (prefetchTile !=
            currentId.tile)
        {
            hasPrefetchTile = true;

            AddUnique(
                desired,
                prefetchTile);
        }
    }

    stats_.currentTile =
        currentId.tile;

    stats_.desiredRegions =
        static_cast<u32>(
            desired.size());

    stats_.readyRegions = 0;
    stats_.pendingRegions = 0;
    stats_.requestsLastUpdate = 0;
    stats_.prefetchRequestsLastUpdate = 0;

    for (const world::PlanetTileId tile :
         desired)
    {
        const DerivedTerrainRegionId id =
            cache_.IdForTile(tile);

        if (cache_.TryGet(id))
        {
            ++stats_.readyRegions;
            continue;
        }

        if (cache_.IsPending(id))
        {
            ++stats_.pendingRegions;
            continue;
        }

        if (!cache_.Request(id))
        {
            continue;
        }

        ++stats_.pendingRegions;
        ++stats_.requestsLastUpdate;
        ++stats_.cumulativeRequests;

        if (hasPrefetchTile &&
            tile == prefetchTile)
        {
            ++stats_.prefetchRequestsLastUpdate;
            ++stats_.cumulativePrefetchRequests;
        }
    }
}

const DerivedTerrainRegionStreamerStats&
DerivedTerrainRegionStreamer::Stats()
    const noexcept
{
    return stats_;
}
} // namespace orbit::terrain_region
