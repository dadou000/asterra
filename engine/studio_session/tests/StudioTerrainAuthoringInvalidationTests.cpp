#include <orbit/studio_session/StudioTerrainAuthoringInvalidation.hpp>

#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <span>
#include <string>

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

    {
        const orbit::math::Double3 center{
            0.0, 1.0, 0.0
        };

        const auto requests =
            orbit::studio_session::
                BuildTerrainAuthoringInvalidations(
                    planet,
                    std::span{
                        &center,
                        std::size_t{1U}},
                    750.0,
                    9U,
                    0U,
                    orbit::terrain_dependency::
                        TerrainChangeKind::
                            BiomePlacement);

        Check(requests.size() == 1U);
        Check(
            requests.front().kind ==
            orbit::terrain_dependency::
                TerrainChangeKind::
                    BiomePlacement);
        Check(!requests.front().scope.global);
        Check(
            requests.front().
                scope.downstreamRadiusTiles ==
            0U);

        const auto dirty =
            orbit::terrain_dependency::
                TerrainDependencyGraph::
                    ProductsForChange(
                        orbit::terrain_dependency::
                            TerrainChangeKind::
                                BiomePlacement);

        const auto expected =
            orbit::terrain_dependency::ProductBit(
                orbit::terrain_dependency::
                    TerrainDependencyProduct::
                        BiomeWeights) |
            orbit::terrain_dependency::ProductBit(
                orbit::terrain_dependency::
                    TerrainDependencyProduct::
                        SurfaceMaterial) |
            orbit::terrain_dependency::ProductBit(
                orbit::terrain_dependency::
                    TerrainDependencyProduct::
                        Scatter);

        Check(dirty == expected);
    }

    {
        orbit::terrain_impacts::ImpactFieldDefinition recipe{
            .id = {.high = 1U, .low = 2U},
            .planet = planet.id,
            .name = "Local crater edit"};
        recipe.authoredImpacts.push_back({
            .id = {.high = 3U, .low = 4U},
            .centerUnitDirection = {1.0, 0.0, 0.0},
            .radiusMeters = 12'000.0,
            .ageOrder = 10U});
        const std::string newRecipe =
            orbit::terrain_impacts::SerializeImpactFieldToml(recipe);
        const auto local =
            orbit::studio_session::BuildImpactHistoryInvalidations(
                planet, {}, newRecipe, 10U);
        Check(!local.empty());
        for (const auto& request : local)
        {
            Check(!request.scope.global);
            Check(request.scope.planet == planet.id);
        }

        auto moved = recipe;
        moved.authoredImpacts.front().centerUnitDirection = {0.0, 0.0, 1.0};
        const auto movement = orbit::studio_session::BuildImpactHistoryInvalidations(
            planet, newRecipe,
            orbit::terrain_impacts::SerializeImpactFieldToml(moved), 10U);
        for (const orbit::math::Double3 point : {
                 orbit::math::Double3{1.0, 0.0, 0.0},
                 orbit::math::Double3{0.0, 0.0, 1.0}})
        {
            const auto tile = orbit::world::TileForDirection(point, 10U);
            Check(std::any_of(movement.begin(), movement.end(), [&](const auto& request)
                { return request.scope.global || request.scope.center == tile; }));
        }

        auto rays = recipe;
        auto& rayCrater = rays.authoredImpacts.front();
        rayCrater.rayStrength = 1.0;
        rayCrater.rayCount = 8U;
        rayCrater.rayExtentRadii = 3.0;
        const std::string rayRecipe = orbit::terrain_impacts::SerializeImpactFieldToml(rays);
        rayCrater.rayIrregularity = 0.3;
        const auto shapeEdit = orbit::studio_session::BuildImpactHistoryInvalidations(
            planet, rayRecipe, orbit::terrain_impacts::SerializeImpactFieldToml(rays), 10U);
        Check(!shapeEdit.empty() && !shapeEdit.front().scope.global);
        rayCrater.rayExtentRadii = 16.0;
        const auto reachEdit = orbit::studio_session::BuildImpactHistoryInvalidations(
            planet, rayRecipe, orbit::terrain_impacts::SerializeImpactFieldToml(rays), 10U);
        Check(reachEdit.size() == 1U && reachEdit.front().scope.global);
        // A ray beyond the 64-tile local scope must use a full invalidation;
        // truncating its influence would preserve stale physical pages.

        recipe.surfaceAgeYears = 3.8e9;
        const std::string agedRecipe =
            orbit::terrain_impacts::SerializeImpactFieldToml(recipe);
        const auto global =
            orbit::studio_session::BuildImpactHistoryInvalidations(
                planet, newRecipe, agedRecipe, 10U);
        Check(global.size() == 1U && global.front().scope.global);
    }

    return 0;
}
