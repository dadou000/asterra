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

        const auto processObject =
            commands.CreateObject(
                orbit::world_model::
                    kTerrainProcessAssetType,
                "Terrain Process Settings",
                terrainObject,
                50);

        commands.SetProperty(
            processObject,
            orbit::world_model::
                kProcessStreamPowerEnabled,
            false);
        commands.SetProperty(
            processObject,
            orbit::world_model::
                kProcessHydraulicRainfall,
            0.00042);
        commands.SetProperty(
            processObject,
            orbit::world_model::
                kProcessAeolianCapacity,
            0.075);
        commands.SetProperty(
            processObject,
            orbit::world_model::
                kProcessRiverMeandersEnabled,
            false);
        commands.SetProperty(
            processObject,
            orbit::world_model::
                kProcessCoastalEnabled,
            false);

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

        const auto* processes =
            surfaces.ProcessesForBody(
                *bodyId);

        if (processes == nullptr ||
            processes->streamPowerEnabled ||
            processes->hydraulic.rainfallMetersPerSecond !=
                0.00042 ||
            processes->aeolian.capacityCoefficient !=
                0.075 ||
            processes->rivers.enableMeanders ||
            processes->coastal.enabled ||
            !processes->IsValid())
        {
            return 26;
        }

        const auto canyon=commands.CreateObject(
            orbit::world_model::kTerrainConstraintType,
            "Canyon",terrainObject,4'000);
        commands.SetProperty(canyon,orbit::world_model::kTerrainConstraintChannel,orbit::i64{0});
        commands.SetProperty(canyon,orbit::world_model::kTerrainConstraintShape,orbit::i64{1});
        commands.SetProperty(canyon,orbit::world_model::kTerrainConstraintMode,orbit::i64{1});
        commands.SetProperty(canyon,orbit::world_model::kTerrainConstraintHalfWidth,500.0);
        commands.SetProperty(canyon,orbit::world_model::kTerrainConstraintFalloff,250.0);
        commands.SetProperty(canyon,orbit::world_model::kTerrainConstraintValue,300.0);

        const auto pointA=commands.CreateObject(
            orbit::world_model::kTerrainConstraintControlPointType,"Point 1",canyon,0);
        const auto pointB=commands.CreateObject(
            orbit::world_model::kTerrainConstraintControlPointType,"Point 2",canyon,10);
        commands.SetProperty(pointA,orbit::world_model::kTerrainConstraintPointDirection,
            orbit::math::Double3{1.0,0.0,0.0});
        commands.SetProperty(pointB,orbit::world_model::kTerrainConstraintPointDirection,
            orbit::math::Double3{0.9999,0.01,0.0});

        if(!universe.RebuildIfChanged(objects)) return 5;
        const auto bodyWithConstraint=universe.BodyForObject(bodyObject);
        if(!bodyWithConstraint.has_value()) return 6;

        const auto constrainedStats=surfaces.Rebuild(objects,universe);
        const auto* authored=surfaces.ConstraintsForBody(*bodyWithConstraint);
        if(authored==nullptr||authored->height.constraints.size()!=1U||
           constrainedStats.terrainConstraints!=1U) return 7;

        const auto planet=surfaces.Registry().SphericalPlanetDefinition(*bodyWithConstraint);
        if(!planet.has_value()) return 8;

        const auto authoredSample=orbit::surface_authoring::EvaluateTerrainConstraintSet(
            *authored,*planet,{
                .planet=planet->id,
                .unitDirection={1.0,0.0,0.0},
                .radialOffsetMeters=0.0
            });
        if(authoredSample.heightMeters>-299.0) return 9;

        const auto* baseOnlyBiomes =
            surfaces.BiomesForBody(
                *bodyWithConstraint);

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

        const auto forcedForestObject =
            commands.CreateObject(
                orbit::world_model::kBiomeAssetType,
                "Authored Cold Forest",
                terrainObject);

        commands.SetProperty(
            forcedForestObject,
            orbit::world_model::kBiomePlacementMode,
            orbit::i64{2});

        commands.SetProperty(
            forcedForestObject,
            orbit::world_model::kBiomeMinimumResolvedWeight,
            0.01);

        const auto temperatureSelector =
            commands.CreateObject(
                orbit::world_model::kBiomeSelectorType,
                "Cold climate selector",
                forcedForestObject,
                0);

        commands.SetProperty(
            temperatureSelector,
            orbit::world_model::kBiomeSelectorField,
            orbit::i64{0});

        commands.SetProperty(
            temperatureSelector,
            orbit::world_model::kBiomeSelectorMinimum,
            -20.0);

        commands.SetProperty(
            temperatureSelector,
            orbit::world_model::kBiomeSelectorMaximum,
            0.0);

        commands.SetProperty(
            temperatureSelector,
            orbit::world_model::kBiomeSelectorUpperFalloff,
            5.0);

        const auto authoredMask =
            commands.CreateObject(
                orbit::world_model::kBiomeAuthoredMaskType,
                "Force forest",
                forcedForestObject,
                10);

        commands.SetProperty(
            authoredMask,
            orbit::world_model::kBiomeMaskOperation,
            orbit::i64{2});

        commands.SetProperty(
            authoredMask,
            orbit::world_model::kBiomeMaskGlobal,
            true);

        commands.SetProperty(
            authoredMask,
            orbit::world_model::kBiomeMaskValue,
            1.0);

        commands.SetProperty(
            authoredMask,
            orbit::world_model::kBiomeMaskOpacity,
            1.0);

        const auto mossLayer =
            commands.CreateObject(
                orbit::world_model::kBiomeSurfaceLayerType,
                "Wet basalt moss",
                forcedForestObject,
                20);

        commands.SetProperty(
            mossLayer,
            orbit::world_model::kBiomeSurfaceLayerKind,
            orbit::i64{1});

        commands.SetProperty(
            mossLayer,
            orbit::world_model::kBiomeSurfaceLayerStrength,
            0.7);

        commands.SetProperty(
            mossLayer,
            orbit::world_model::kBiomeSurfaceLayerCompatibility,
            orbit::i64{1});

        commands.SetProperty(
            mossLayer,
            orbit::world_model::kBiomeSurfaceLayerMoistureMin,
            0.5);

        const auto treeScatter =
            commands.CreateObject(
                orbit::world_model::kBiomeScatterRuleType,
                "Canopy trees",
                forcedForestObject,
                30);

        commands.SetProperty(
            treeScatter,
            orbit::world_model::kBiomeScatterKind,
            orbit::i64{0});
        commands.SetProperty(
            treeScatter,
            orbit::world_model::kBiomeScatterDensity,
            0.0125);
        commands.SetProperty(
            treeScatter,
            orbit::world_model::kBiomeScatterSpacing,
            3.0);
        commands.SetProperty(
            treeScatter,
            orbit::world_model::kBiomeScatterSeedSalt,
            orbit::i64{77});
        commands.SetProperty(
            treeScatter,
            orbit::world_model::kBiomeScatterRequiresSoil,
            true);
        commands.SetProperty(
            treeScatter,
            orbit::world_model::kBiomeScatterMinimumSoilDepth,
            0.15);
        commands.SetProperty(
            treeScatter,
            orbit::world_model::kBiomeScatterSlopeMax,
            50.0);
        commands.SetProperty(
            treeScatter,
            orbit::world_model::kBiomeScatterMoistureMin,
            0.1);
        commands.SetProperty(
            treeScatter,
            orbit::world_model::kBiomeScatterMoistureMax,
            0.8);
        commands.SetProperty(
            treeScatter,
            orbit::world_model::kBiomeScatterScaleMin,
            0.8);
        commands.SetProperty(
            treeScatter,
            orbit::world_model::kBiomeScatterScaleMax,
            1.2);

        if (!universe.RebuildIfChanged(objects))
        {
            return 18;
        }

        static_cast<void>(
            surfaces.Rebuild(
                objects,
                universe));

        const auto* forcedService =
            surfaces.BiomesForBody(
                *universe.BodyForObject(
                    bodyObject));

        if (forcedService == nullptr ||
            forcedService->Definitions().size() != 2U)
        {
            return 19;
        }

        const auto forcedDefinitions =
            forcedService->Definitions();

        const auto forcedForest =
            forcedDefinitions.size() > 1U
                ? forcedDefinitions[1]
                : orbit::terrain_biome::BiomeDefinition{};

        if (forcedForest.placement.mode !=
                orbit::terrain_biome::
                    BiomePlacementMode::
                        AutomaticAndAuthored ||
            forcedForest.placement.selectors.size() != 1U ||
            forcedForest.placement.authoredMasks.size() != 1U ||
            forcedForest.surface.layers.size() != 1U ||
            forcedForest.scatter.layers.size() != 1U ||
            forcedForest.scatter.layers.front().kind !=
                orbit::terrain_biome::BiomeScatterKind::Tree ||
            forcedForest.scatter.layers.front().densityPerSquareMeter !=
                0.0125F ||
            forcedForest.scatter.layers.front().seedSalt != 77U ||
            forcedForest.surface.layers.front().kind !=
                orbit::terrain_biome::BiomeSurfaceLayerKind::Moss ||
            forcedForest.surface.layers.front().strength != 0.7F)
        {
            return 20;
        }

        const auto persistedMaskId =
            forcedForest.
                placement.
                authoredMasks.
                front().
                id;

        orbit::terrain_biome::BiomePlacementContext hotClimate{};
        hotClimate.temperatureC = 45.0;

        const auto forcedEvaluation =
            forcedService->EvaluatePlacement(
                forcedForest,
                hotClimate);

        if (forcedEvaluation.automaticWeight != 0.0 ||
            forcedEvaluation.finalWeight != 1.0)
        {
            return 21;
        }

        // Regenerate terrain and independently edit the procedural climate
        // selector. The authored semantic child must survive with the same
        // stable mask identity and continue to force the biome.
        commands.SetProperty(
            terrainObject,
            orbit::world_model::kTerrainMacroAmplitudeMeters,
            725.0);

        commands.SetProperty(
            temperatureSelector,
            orbit::world_model::kBiomeSelectorMinimum,
            30.0);

        commands.SetProperty(
            temperatureSelector,
            orbit::world_model::kBiomeSelectorMaximum,
            50.0);

        if (!universe.RebuildIfChanged(objects))
        {
            return 22;
        }

        static_cast<void>(
            surfaces.Rebuild(
                objects,
                universe));

        const auto* regeneratedService =
            surfaces.BiomesForBody(
                *universe.BodyForObject(
                    bodyObject));

        if (regeneratedService == nullptr ||
            regeneratedService->Definitions().size() != 2U)
        {
            return 23;
        }

        const auto regeneratedForest =
            regeneratedService->
                Definitions()[1];

        if (regeneratedForest.
                placement.
                authoredMasks.
                size() != 1U ||
            regeneratedForest.
                placement.
                authoredMasks.
                front().
                id !=
                persistedMaskId ||
            regeneratedForest.
                placement.
                selectors.
                size() != 1U ||
            regeneratedForest.
                placement.
                selectors.
                front().
                minimum != 30.0 ||
            regeneratedForest.
                surface.
                layers.
                size() != 1U ||
            regeneratedForest.
                scatter.
                layers.
                size() != 1U ||
            regeneratedForest.
                scatter.
                layers.
                front().
                seedSalt != 77U ||
            regeneratedForest.
                surface.
                layers.
                front().
                kind !=
                orbit::terrain_biome::
                    BiomeSurfaceLayerKind::
                        Moss)
        {
            return 24;
        }

        const auto regeneratedEvaluation =
            regeneratedService->
                EvaluatePlacement(
                    regeneratedForest,
                    hotClimate);

        if (regeneratedEvaluation.automaticWeight != 1.0 ||
            regeneratedEvaluation.finalWeight != 1.0)
        {
            return 25;
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
