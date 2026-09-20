#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/surface_model/SurfaceComposition.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/world_model/UniverseComposition.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
using namespace orbit;

void Require(
    const bool condition,
    const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void TestDefaultRockyPlanetComposition()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-v004-m31-entry-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    try
    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "V0.0.4 M31 Integration Entry");

        documents::WorldDatabase world(
            project.StartupWorldPath());

        schema::SchemaRegistry schemas;
        world_model::RegisterSchemas(
            schemas);

        scene::ObjectStore objects(
            world);

        commands::CommandService commands(
            objects,
            schemas);

        const auto worldObject =
            commands.CreateObject(
                world_model::kWorldType,
                "World");

        const auto systemObject =
            commands.CreateObject(
                world_model::kCelestialSystemType,
                "Helion",
                worldObject);

        const auto bodyObject =
            commands.CreateObject(
                world_model::kCelestialBodyType,
                "Asterra",
                systemObject);

        const auto terrainObject =
            commands.CreateObject(
                world_model::kTerrainSurfaceType,
                "Asterra Terrain",
                bodyObject);

        // Deliberately set no body-radius, terrain recipe, biome, or process
        // properties. M31 must prove the ordinary default path is complete.
        world_model::UniverseComposition universe;

        const auto universeStats =
            universe.Rebuild(
                objects);

        Require(
            universeStats.bodies == 1U,
            "M31 entry: default semantic rocky body did not compose.");

        const auto bodyId =
            universe.BodyForObject(
                bodyObject);

        Require(
            bodyId.has_value(),
            "M31 entry: default rocky body has no stable runtime BodyId.");

        surface_model::SurfaceComposition
            surfaces;

        const auto surfaceStats =
            surfaces.Rebuild(
                objects,
                universe);

        Require(
            surfaceStats.terrainSurfaces == 1U &&
            surfaceStats.biomeServices == 1U &&
            surfaceStats.biomeDefinitions == 1U,
            "M31 entry: default rocky terrain must compose one terrain capability and one BaseBiome service.");

        Require(
            surfaces.BodyForTerrainObject(
                terrainObject) ==
                bodyId &&
            surfaces.TerrainObjectForBody(
                *bodyId) ==
                terrainObject,
            "M31 entry: semantic terrain/body mapping is not stable.");

        const auto* capability =
            surfaces.Registry().
                FindTerrainSurface(
                    *bodyId);

        Require(
            capability != nullptr &&
            capability->terrain != nullptr,
            "M31 entry: default rocky body is missing terrain capability.");

        const auto* analytic =
            dynamic_cast<
                const terrain::AnalyticTerrainSource*>(
                    capability->terrain.get());

        Require(
            analytic != nullptr,
            "M31 entry: default terrain capability is not the production analytic terrain source.");

        const terrain::AnalyticTerrainDesc defaults{};

        const auto& description =
            analytic->Description();

        Require(
            description.seed == defaults.seed &&
            description.macroAmplitudeMeters ==
                defaults.macroAmplitudeMeters &&
            description.macroWavelengthMeters ==
                defaults.macroWavelengthMeters &&
            description.detailAmplitudeMeters ==
                defaults.detailAmplitudeMeters &&
            description.detailWavelengthMeters ==
                defaults.detailWavelengthMeters &&
            description.detailOctaves ==
                defaults.detailOctaves &&
            description.maximumElevationAboveSeaLevelMeters ==
                defaults.maximumElevationAboveSeaLevelMeters,
            "M31 entry: no-input terrain surface did not resolve the production default recipe.");

        const auto* biomeService =
            surfaces.BiomesForBody(
                *bodyId);

        Require(
            biomeService != nullptr,
            "M31 entry: default rocky terrain has no BaseBiome service.");

        const auto definitions =
            biomeService->Definitions();

        const auto resolvedBase =
            biomeService->Resolve({});

        Require(
            definitions.size() == 1U &&
            definitions.front().id ==
                biomeService->BaseBiome().id &&
            resolvedBase.size() == 1U &&
            resolvedBase.front().base &&
            resolvedBase.front().weight == 1.0F,
            "M31 entry: BaseBiome is not the complete no-input fallback.");

        const auto planet =
            surfaces.Registry().
                SphericalPlanetDefinition(
                    *bodyId);

        Require(
            planet.has_value() &&
            planet->radiusMeters ==
                6'000'000.0,
            "M31 entry: default rocky body did not resolve the production spherical planet defaults.");

        constexpr std::array<
            math::Double3,
            4U>
            directions{{
                {1.0, 0.0, 0.0},
                {0.0, 1.0, 0.0},
                {0.0, 0.0, 1.0},
                {0.5773502691896258,
                 0.5773502691896258,
                 0.5773502691896258}
            }};

        for (const auto& direction :
             directions)
        {
            const auto sample =
                capability->terrain->Sample({
                    .unitDirection =
                        math::Normalize(
                            direction),
                    .footprintMeters =
                        32.0,
                    .planet =
                        planet->id,
                    .radialOffsetMeters =
                        0.0
                });

            const f32 biomeTotal =
                sample.biomes.ocean +
                sample.biomes.desert +
                sample.biomes.grassland +
                sample.biomes.temperateForest +
                sample.biomes.borealForest +
                sample.biomes.tundra +
                sample.biomes.alpine +
                sample.biomes.wetland;

            Require(
                std::isfinite(
                    sample.elevationMeters) &&
                std::isfinite(
                    sample.coarseElevationMeters) &&
                std::isfinite(
                    sample.climate.temperatureC) &&
                std::isfinite(
                    sample.climate.humidity) &&
                std::isfinite(
                    sample.climate.precipitation) &&
                std::isfinite(
                    sample.climate.continentality) &&
                std::isfinite(
                    sample.standingWaterDepthMeters) &&
                sample.standingWaterDepthMeters >=
                    0.0 &&
                std::abs(
                    static_cast<f64>(
                        biomeTotal) -
                    1.0) <=
                    1.0e-4,
                "M31 entry: default terrain sampling did not produce a finite complete surface sample.");
        }

        world.Checkpoint();
    }
    catch (...)
    {
        std::filesystem::remove_all(
            root);
        throw;
    }

    std::filesystem::remove_all(
        root);
}
} // namespace

int main()
{
    try
    {
        TestDefaultRockyPlanetComposition();

        std::cout
            << "Orbit V0.0.4 M31 integration entry: "
            << "default rocky-planet composition passed.\n";

        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "Orbit V0.0.4 M31 integration entry failed: "
            << error.what()
            << '\n';

        return EXIT_FAILURE;
    }
}
