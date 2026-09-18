#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/surface_model/SurfaceComposition.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/world_model/UniverseComposition.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <stdexcept>

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-surface-composition-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Surface Composition Test");
        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());
        orbit::schema::SchemaRegistry schemas;
        orbit::world_model::RegisterSchemas(schemas);
        orbit::scene::ObjectStore objects(world);
        orbit::commands::CommandService commands(objects, schemas);

        const auto worldObject = commands.CreateObject(
            orbit::world_model::kWorldType,
            "World");
        const auto systemObject = commands.CreateObject(
            orbit::world_model::kCelestialSystemType,
            "Helion",
            worldObject);
        const auto bodyObject = commands.CreateObject(
            orbit::world_model::kCelestialBodyType,
            "Asterra",
            systemObject);
        const auto terrainObject = commands.CreateObject(
            orbit::world_model::kTerrainSurfaceType,
            "Asterra Terrain",
            bodyObject);

        commands.SetProperty(
            bodyObject,
            orbit::world_model::kBodyRadius,
            6'400'000.0);
        commands.SetProperty(
            terrainObject,
            orbit::world_model::kTerrainSeed,
            orbit::i64{1234});
        commands.SetProperty(
            terrainObject,
            orbit::world_model::kTerrainMacroAmplitudeMeters,
            500.0);
        commands.SetProperty(
            terrainObject,
            orbit::world_model::kTerrainDetailOctaves,
            orbit::i64{6});

        orbit::world_model::UniverseComposition universe;
        const auto universeStats = universe.Rebuild(objects);

        if (universeStats.bodies != 1U)
        {
            return 1;
        }

        orbit::surface_model::SurfaceComposition surfaces;
        const auto surfaceStats = surfaces.Rebuild(objects, universe);

        if (surfaceStats.terrainSurfaces != 1U ||
            surfaceStats.biomeServices != 1U ||
            surfaceStats.biomeDefinitions != 1U ||
            surfaceStats.sourceRevision != objects.Revision())
        {
            return 2;
        }

        const auto bodyId = universe.BodyForObject(bodyObject);
        const auto terrainBody = surfaces.BodyForTerrainObject(terrainObject);

        if (!bodyId.has_value() ||
            terrainBody != bodyId ||
            surfaces.TerrainObjectForBody(*bodyId) != terrainObject)
        {
            return 3;
        }

        const auto* capability =
            surfaces.Registry().FindTerrainSurface(*bodyId);
        const auto* analytic = capability != nullptr
            ? dynamic_cast<const orbit::terrain::AnalyticTerrainSource*>(
                  capability->terrain.get())
            : nullptr;

        if (analytic == nullptr ||
            analytic->Description().seed != 1234U ||
            analytic->Description().macroAmplitudeMeters != 500.0 ||
            analytic->Description().detailOctaves != 6U)
        {
            return 4;
        }

        const auto* baseOnlyBiomes =
            surfaces.BiomesForBody(
                *bodyId);

        if (baseOnlyBiomes == nullptr ||
            baseOnlyBiomes->Definitions().size() != 1U ||
            baseOnlyBiomes->Resolve({}).size() != 1U ||
            !baseOnlyBiomes->Resolve({}).front().base ||
            baseOnlyBiomes->Resolve({}).front().weight != 1.0F)
        {
            return 11;
        }

        const auto forestObject =
            commands.CreateObject(
                orbit::world_model::kBiomeAssetType,
                "Temperate Forest",
                terrainObject);

        commands.SetProperty(
            forestObject,
            orbit::world_model::kBiomeMinimumResolvedWeight,
            0.25);

        commands.SetProperty(
            forestObject,
            orbit::world_model::kBiomeScatterDensityMultiplier,
            1.4);

        if (!universe.RebuildIfChanged(objects))
        {
            return 12;
        }

        const auto bodyAfterBiomeAdd =
            universe.BodyForObject(
                bodyObject);

        if (!bodyAfterBiomeAdd.has_value() ||
            *bodyAfterBiomeAdd != *bodyId)
        {
            return 13;
        }

        const auto withBiomeStats =
            surfaces.Rebuild(
                objects,
                universe);

        const auto* withBiome =
            surfaces.BiomesForBody(
                *bodyAfterBiomeAdd);

        if (withBiomeStats.biomeServices != 1U ||
            withBiomeStats.biomeDefinitions != 2U ||
            withBiome == nullptr ||
            withBiome->Definitions().size() != 2U)
        {
            return 14;
        }

        const auto definitions =
            withBiome->Definitions();

        const auto forest =
            definitions.size() > 1U
                ? definitions[1]
                : orbit::terrain_biome::BiomeDefinition{};

        if (forest.name != "Temperate Forest" ||
            forest.placement.minimumResolvedWeight != 0.25F ||
            forest.scatter.densityMultiplier != 1.4F)
        {
            return 15;
        }

        commands.DeleteObject(
            forestObject);

        if (!universe.RebuildIfChanged(objects))
        {
            return 16;
        }

        static_cast<void>(
            surfaces.Rebuild(
                objects,
                universe));

        const auto* afterBiomeDelete =
            surfaces.BiomesForBody(
                *universe.BodyForObject(
                    bodyObject));

        if (afterBiomeDelete == nullptr ||
            afterBiomeDelete->Definitions().size() != 1U ||
            afterBiomeDelete->Resolve({}).size() != 1U ||
            afterBiomeDelete->Resolve({}).front().weight != 1.0F)
        {
            return 17;
        }

        commands.SetProperty(
            terrainObject,
            orbit::world_model::kTerrainMacroAmplitudeMeters,
            750.0);

        if (!universe.RebuildIfChanged(objects))
        {
            return 5;
        }

        const auto stableBodyId = universe.BodyForObject(bodyObject);

        if (!stableBodyId.has_value() || *stableBodyId != *bodyId)
        {
            return 6;
        }

        static_cast<void>(surfaces.Rebuild(objects, universe));
        capability = surfaces.Registry().FindTerrainSurface(*stableBodyId);
        analytic = capability != nullptr
            ? dynamic_cast<const orbit::terrain::AnalyticTerrainSource*>(
                  capability->terrain.get())
            : nullptr;

        if (analytic == nullptr ||
            analytic->Description().macroAmplitudeMeters != 750.0)
        {
            return 7;
        }

        commands.SetProperty(
            bodyObject,
            orbit::world_model::kBodyEllipsoidEnabled,
            true);
        commands.SetProperty(
            bodyObject,
            orbit::world_model::kBodyPolarRadius,
            6'350'000.0);
        static_cast<void>(universe.Rebuild(objects));

        bool rejectedEllipsoid = false;
        try
        {
            static_cast<void>(surfaces.Rebuild(objects, universe));
        }
        catch (const std::runtime_error&)
        {
            rejectedEllipsoid = true;
        }

        if (!rejectedEllipsoid)
        {
            return 8;
        }

        bool registryInvalidated = false;
        try
        {
            static_cast<void>(surfaces.Registry());
        }
        catch (const std::logic_error&)
        {
            registryInvalidated = true;
        }

        if (!registryInvalidated)
        {
            return 9;
        }

        commands.SetProperty(
            bodyObject,
            orbit::world_model::kBodyEllipsoidEnabled,
            false);
        static_cast<void>(universe.Rebuild(objects));
        static_cast<void>(surfaces.Rebuild(objects, universe));

        if (surfaces.Registry().FindTerrainSurface(
                *universe.BodyForObject(bodyObject)) == nullptr)
        {
            return 10;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
