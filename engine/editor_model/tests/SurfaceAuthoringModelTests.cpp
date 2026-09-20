#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <variant>

int main()
{
    using namespace orbit;

    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-m28-surface-authoring-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project = documents::ProjectDocument::Create(
            root,
            "M28 Surface Authoring Test");
        documents::WorldDatabase world(project.StartupWorldPath());

        schema::SchemaRegistry schemas;
        world_model::RegisterSchemas(schemas);
        scene::ObjectStore objects(world);
        commands::CommandService commands(objects, schemas);
        selection::SelectionService selection;

        const auto worldObject = commands.CreateObject(world_model::kWorldType, "World");
        const auto systemObject = commands.CreateObject(world_model::kCelestialSystemType, "Helion", worldObject);
        const auto bodyObject = commands.CreateObject(world_model::kCelestialBodyType, "Asterra", systemObject);
        const auto terrainObject = commands.CreateObject(world_model::kTerrainSurfaceType, "Terrain Surface", bodyObject);

        const scene::ObjectId selected[] = {bodyObject};
        selection.Set(selected);

        editor_model::SurfaceAuthoringModel model(objects, commands, selection);
        const auto rocky = model.SelectedRockyBody();

        if (!rocky.has_value() || rocky->body != bodyObject ||
            rocky->terrain != terrainObject || rocky->bodyName != "Asterra")
        {
            return 1;
        }

        auto relief = model.Relief(terrainObject);
        relief.macroAmplitudeMeters = 2'500.0;
        relief.detailOctaves = 12;
        model.SetRelief(terrainObject, relief);

        const auto storedAmplitude = objects.GetProperty(
            terrainObject,
            world_model::kTerrainMacroAmplitudeMeters);

        if (!storedAmplitude.has_value() ||
            std::get<f64>(*storedAmplitude) != 2'500.0)
        {
            return 2;
        }

        if (model.ProcessSettingsObject(
                terrainObject).
            has_value())
        {
            return 19;
        }

        const auto processObject =
            model.EnsureProcessSettings(
                terrainObject);

        if (!processObject.IsValid() ||
            model.Counts(
                terrainObject).
                processAssets != 1U)
        {
            return 20;
        }

        const auto defaultProcesses =
            model.ProcessSettings(
                terrainObject);

        if (!defaultProcesses.IsValid() ||
            !defaultProcesses.aeolianEnabled ||
            defaultProcesses.aeolian.capacityCoefficient !=
                0.030)
        {
            return 21;
        }

        auto editedProcesses =
            defaultProcesses;

        editedProcesses.aeolianEnabled =
            false;
        editedProcesses.aeolian.capacityCoefficient =
            0.045;
        editedProcesses.hydraulic.rainfallMetersPerSecond =
            0.00035;
        editedProcesses.rivers.enableMeanders =
            false;
        editedProcesses.coastal.enabled =
            false;

        model.SetProcessSettings(
            terrainObject,
            editedProcesses);

        const auto storedProcesses =
            model.ProcessSettings(
                terrainObject);

        if (storedProcesses.aeolianEnabled ||
            storedProcesses.aeolian.capacityCoefficient !=
                0.045 ||
            storedProcesses.hydraulic.rainfallMetersPerSecond !=
                0.00035 ||
            storedProcesses.rivers.enableMeanders ||
            storedProcesses.coastal.enabled)
        {
            return 22;
        }

        auto invalidProcesses =
            storedProcesses;

        invalidProcesses.hydraulic.timeStepSeconds =
            0.0;

        bool rejectedInvalidProcess = false;

        try
        {
            model.SetProcessSettings(
                terrainObject,
                invalidProcesses);
        }
        catch (const std::invalid_argument&)
        {
            rejectedInvalidProcess = true;
        }

        if (!rejectedInvalidProcess ||
            model.ProcessSettings(
                terrainObject).
                hydraulic.timeStepSeconds !=
            storedProcesses.hydraulic.timeStepSeconds)
        {
            return 23;
        }

        commands.Undo();

        const auto undoneProcesses =
            model.ProcessSettings(
                terrainObject);

        if (!undoneProcesses.aeolianEnabled ||
            undoneProcesses.aeolian.capacityCoefficient !=
                defaultProcesses.aeolian.capacityCoefficient ||
            !undoneProcesses.rivers.enableMeanders ||
            !undoneProcesses.coastal.enabled)
        {
            return 24;
        }

        commands.Redo();

        if (model.ProcessSettings(
                terrainObject).
                aeolian.capacityCoefficient !=
            0.045)
        {
            return 25;
        }

        const auto desert = model.AddBiome(terrainObject, "Desert");
        auto biomes = model.Biomes(terrainObject);

        if (biomes.size() != 1U || biomes.front().id != desert ||
            biomes.front().selectors != 3U)
        {
            return 3;
        }

        auto preferences = model.CommonPreferences(desert);
        preferences.temperature = {22.0, 55.0, 8.0, 8.0};
        preferences.moisture = {0.0, 0.25, 0.05, 0.10};
        preferences.elevation = {-500.0, 2'500.0, 250.0, 600.0};
        model.SetCommonPreferences(desert, preferences);

        const auto storedPreferences = model.CommonPreferences(desert);
        if (storedPreferences.temperature.minimum != 22.0 ||
            storedPreferences.moisture.maximum != 0.25 ||
            storedPreferences.elevation.maximum != 2'500.0)
        {
            return 4;
        }

        const auto mask = model.PaintLocalOverride(
            desert,
            {2.0, 0.0, 0.0},
            250.0,
            1'000.0,
            0.9,
            0.75);

        const auto masks = model.Masks(desert);
        if (masks.size() != 1U || masks.front().id != mask ||
            masks.front().operation != terrain_biome::BiomeAuthoredWeightOperation::Replace ||
            masks.front().centerUnitDirection.x != 1.0 ||
            masks.front().outerRadiusMeters != 1'000.0 ||
            masks.front().value != 0.9)
        {
            return 5;
        }

        const auto placementMode = objects.GetProperty(
            desert,
            world_model::kBiomePlacementMode);

        if (!placementMode.has_value() ||
            std::get<i64>(*placementMode) != 2)
        {
            return 6;
        }

        const auto addedMask =
            model.PaintBiomeMask(
                desert,
                terrain_biome::
                    BiomeAuthoredWeightOperation::Add,
                {0.0, 1.0, 0.0},
                100.0,
                500.0,
                0.25,
                0.5);

        const auto subtractedMask =
            model.PaintBiomeMask(
                desert,
                terrain_biome::
                    BiomeAuthoredWeightOperation::Subtract,
                {0.0, 0.0, 1.0},
                75.0,
                400.0,
                0.10,
                1.0);

        const auto operationMasks =
            model.Masks(desert);

        if (operationMasks.size() != 3U ||
            operationMasks[1].id != addedMask ||
            operationMasks[1].operation !=
                terrain_biome::
                    BiomeAuthoredWeightOperation::Add ||
            operationMasks[2].id != subtractedMask ||
            operationMasks[2].operation !=
                terrain_biome::
                    BiomeAuthoredWeightOperation::Subtract)
        {
            return 15;
        }

        commands.Undo();

        if (model.Masks(desert).size() != 2U)
        {
            return 16;
        }

        commands.Redo();

        if (model.Masks(desert).size() != 3U)
        {
            return 17;
        }

        const auto moss = model.AddSurfaceLayer(
            desert,
            terrain_biome::BiomeSurfaceLayerKind::Moss);
        const auto trees = model.AddScatterRule(
            desert,
            terrain_biome::BiomeScatterKind::Tree);

        biomes = model.Biomes(terrainObject);
        if (biomes.front().surfaceLayers != 1U ||
            biomes.front().scatterRules != 1U ||
            !objects.Find(moss).has_value() ||
            !objects.Find(trees).has_value())
        {
            return 7;
        }

        auto settings = model.BiomeSettings(desert);
        settings.materialInfluence = 0.7;
        settings.scatterDensityMultiplier = 1.5;
        settings.aeolianTransport = 0.5;
        model.SetBiomeSettings(desert, settings);

        const auto storedSettings = model.BiomeSettings(desert);
        if (storedSettings.materialInfluence != 0.7 ||
            storedSettings.scatterDensityMultiplier != 1.5 ||
            storedSettings.aeolianTransport != 0.5)
        {
            return 8;
        }

        model.SelectObject(trees);
        const auto descendantSelection = model.SelectedRockyBody();

        if (!descendantSelection.has_value() ||
            descendantSelection->body != bodyObject ||
            descendantSelection->terrain != terrainObject)
        {
            return 9;
        }

        const auto raised=model.AddHeightBrush(terrainObject,{2.0,0.0,0.0},250.0,1'000.0,125.0);
        const auto canyon=model.AddCanyonSpline(terrainObject,
            {{1.0,0.0,0.0},{0.9999,0.01,0.0},{0.9996,0.02,0.0}},300.0,450.0,80.0);

        auto constraints=model.TerrainConstraints(terrainObject);
        if(constraints.size()!=2U||constraints[0].id!=raised||constraints[1].id!=canyon||
           constraints[1].shape!=editor_model::SurfaceTerrainConstraintShape::Spline||
           constraints[1].controlUnitDirections.size()!=3U) return 10;

        commands.Undo();
        if(model.TerrainConstraints(terrainObject).size()!=1U) return 11;
        commands.Redo();
        if(model.TerrainConstraints(terrainObject).size()!=2U) return 12;

        const auto counts = model.Counts(terrainObject);
        if (counts.optionalBiomes != 1U || counts.terrainConstraints != 2U ||
            counts.semanticRevision != objects.Revision()) return 13;

        if (!commands.CanUndo()) return 14;

        world.Checkpoint();

        documents::WorldDatabase reopenedWorld(
            project.StartupWorldPath());

        schema::SchemaRegistry reopenedSchemas;
        world_model::RegisterSchemas(
            reopenedSchemas);

        scene::ObjectStore reopenedObjects(
            reopenedWorld);

        commands::CommandService reopenedCommands(
            reopenedObjects,
            reopenedSchemas);

        selection::SelectionService
            reopenedSelection;

        const scene::ObjectId reopenedSelected[] = {
            desert
        };

        reopenedSelection.Set(
            reopenedSelected);

        editor_model::SurfaceAuthoringModel
            reopenedModel(
                reopenedObjects,
                reopenedCommands,
                reopenedSelection);

        const auto reopenedProcesses =
            reopenedModel.ProcessSettings(
                terrainObject);

        if (reopenedProcesses.aeolianEnabled ||
            reopenedProcesses.aeolian.capacityCoefficient !=
                0.045 ||
            reopenedProcesses.hydraulic.rainfallMetersPerSecond !=
                0.00035 ||
            reopenedProcesses.rivers.enableMeanders ||
            reopenedProcesses.coastal.enabled)
        {
            return 26;
        }

        const auto reopenedMasks =
            reopenedModel.Masks(
                desert);

        if (reopenedMasks.size() != 3U ||
            reopenedMasks[0].operation !=
                terrain_biome::
                    BiomeAuthoredWeightOperation::
                        Replace ||
            reopenedMasks[1].operation !=
                terrain_biome::
                    BiomeAuthoredWeightOperation::
                        Add ||
            reopenedMasks[2].operation !=
                terrain_biome::
                    BiomeAuthoredWeightOperation::
                        Subtract ||
            reopenedMasks[0].outerRadiusMeters !=
                1'000.0 ||
            reopenedMasks[1].opacity !=
                0.5)
        {
            return 18;
        }
    }

    std::filesystem::remove_all(root);
    return 0;
}
