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
    }

    std::filesystem::remove_all(root);
    return 0;
}
