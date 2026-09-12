#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain_region/DerivedTerrainRegionCache.hpp>
#include <orbit/world/Planet.hpp>

namespace orbit::terrain_region
{
struct DerivedTerrainRegionStreamerConfig
{
    u32 neighborhoodRadius{1};
    f64 forwardPrefetchDistanceTiles{1.0};
};

struct DerivedTerrainRegionStreamerStats
{
    world::PlanetTileId currentTile{};

    u32 desiredRegions{0};
    u32 readyRegions{0};
    u32 pendingRegions{0};

    u32 requestsLastUpdate{0};
    u32 prefetchRequestsLastUpdate{0};

    u64 cumulativeRequests{0};
    u64 cumulativePrefetchRequests{0};
};

class DerivedTerrainRegionStreamer
{
public:
    DerivedTerrainRegionStreamer(
        world::PlanetDefinition planet,
        DerivedTerrainRegionCache& cache,
        DerivedTerrainRegionStreamerConfig config = {});

    void Update(
        const math::Double3& observerDirection,
        const math::Double3& surfaceTravelDirection);

    [[nodiscard]] const DerivedTerrainRegionStreamerStats&
    Stats() const noexcept;

private:
    world::PlanetDefinition planet_{};
    DerivedTerrainRegionCache& cache_;
    DerivedTerrainRegionStreamerConfig config_{};
    DerivedTerrainRegionStreamerStats stats_{};
};
} // namespace orbit::terrain_region
