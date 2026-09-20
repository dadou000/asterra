#include <orbit/studio_session/StudioTerrainAuthoringInvalidation.hpp>

#include <cstdlib>
#include <iostream>
#include <span>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr <<
            "Studio terrain authoring invalidation test failed.\n";
        std::exit(1);
    }
}
} // namespace

int main()
{
    const orbit::world::PlanetDefinition planet{
        .radiusMeters = 6'000'000.0,
        .id = {
            .high = 7U,
            .low = 9U
        }
    };

    {
        const orbit::math::Double3 center{
            1.0, 0.0, 0.0
        };

        const auto requests =
            orbit::studio_session::
                BuildTerrainAuthoringInvalidations(
                    planet,
                    std::span{
                        &center,
                        std::size_t{1U}},
                    1'000.0,
                    8U,
                    2U);

        Check(requests.size() == 1U);
        Check(!requests.front().scope.global);
        Check(
            requests.front().kind ==
            orbit::terrain_dependency::
                TerrainChangeKind::
                    TerrainAuthoring);
        Check(
            requests.front().
                scope.radiusTiles +
            requests.front().
                scope.downstreamRadiusTiles <=
            64U);
        Check(
            requests.front().
                scope.center ==
            orbit::world::TileForDirection(
                center,
                8U));
    }

    {
        const orbit::math::Double3 points[]{
            {1.0, 0.0, 0.0},
            {0.98, 0.18, 0.0},
            {0.92, 0.38, 0.0}
        };

        const auto requests =
            orbit::studio_session::
                BuildTerrainAuthoringInvalidations(
                    planet,
                    points,
                    500.0,
                    10U,
                    3U);

        Check(requests.size() > 2U);

        for (const auto& request :
             requests)
        {
            Check(!request.scope.global);
            Check(
                request.scope.
                    radiusTiles +
                request.scope.
                    downstreamRadiusTiles <=
                64U);
            Check(
                request.scope.planet ==
                planet.id);
        }
    }

    return 0;
}
