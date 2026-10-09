#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace
{
using namespace orbit;

void CheckImpl(const bool condition, const char* expression)
{
    if (!condition)
    {
        std::cerr << "Studio terrain bake test failed: " << expression << '\n';
        std::exit(1);
    }
}

#define Check(condition) CheckImpl((condition), #condition)

std::shared_ptr<const terrain::BakedTectonicRasters> SourceBake(
    studio_session::StudioSession& studio,
    const universe::BodyId body)
{
    const auto* capability = studio.World().Surfaces().Registry().FindTerrainSurface(body);
    Check(capability != nullptr);
    const auto* analytic =
        dynamic_cast<const terrain::AnalyticTerrainSource*>(capability->terrain.get());
    Check(analytic != nullptr);
    return analytic->GlobalFields().Description().bakedTectonics;
}

std::string Rpc(studio_session::StudioSession& studio, const std::string& method, const std::string& params)
{
    const auto reply = studio.DispatchRpc(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"" + method + "\",\"params\":" + params + "}");
    Check(reply.has_value());
    return *reply;
}
} // namespace

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("orbit-studio-terrain-bake-" + documents::ProjectId::Random().ToString());
    std::filesystem::remove_all(root);

    {
        auto project = documents::ProjectDocument::Create(root, "Studio Terrain Bake Test");
        studio_session::StudioSession studio(project);
        studio.Viewports().Register(
            "studio.primary", studio_session::ViewportMode::Perspective, true);

        const auto worldObject =
            studio.World().Commands().CreateObject(world_model::kWorldType, "World");
        const scene::ObjectId selected[] = {worldObject};
        studio.World().Selection().Set(selected);
        studio.World().CommandRegistry().Invoke(
            editor_model::authoring_commands::kCreateRockyPlanet,
            {{"name", std::string("Asterra")}});

        // The first tick bakes before anything reads the terrain: the source is
        // composed from the bake, never from the plate model.
        static_cast<void>(studio.Tick());
        Check(studio.ActiveBody().Active().has_value());
        const auto body = studio.ActiveBody().Active()->body;
        const auto terrainObject = studio.World().Surfaces().TerrainObjectForBody(body);
        Check(terrainObject.has_value());

        const auto* services = studio.World().Surfaces().ServicesForBody(body);
        Check(services != nullptr && services->TectonicBake() != nullptr);
        Check(SourceBake(studio, body) == services->TectonicBake());
        const auto first = services->TectonicBake();

        const auto status = studio.TerrainBake().Status(*terrainObject);
        Check(status.has_value());
        Check(status->state == terrain_bake::BakeState::Ready);
        Check(status->activeRecipeHash == status->currentRecipeHash);
        Check(status->riversActive && services->RiverBake() != nullptr);
        Check(std::filesystem::exists(status->path));
        Check(status->path.parent_path().filename() == "Bakes");

        // The RPC and the controller report the same state.
        const std::string reply = Rpc(studio, "terrain.bake_status", "{\"terrain\":\"" + terrainObject->ToString() + "\"}");
        Check(reply.find("\"state\":\"ready\"") != std::string::npos);
        Check(Rpc(studio, "terrain.bake_set", "{\"terrain\":\"" + terrainObject->ToString() + "\",\"resolution\":99999}")
                  .find("error") != std::string::npos);

        // Editing the plate recipe makes the bake stale; the old bake keeps
        // driving the terrain until the new one is ready and swapped in.
        editor_model::SurfaceAuthoringModel model(
            studio.World().Objects(), studio.World().Commands(), studio.World().Selection());
        auto tectonics = model.Tectonics(*terrainObject);
        tectonics.plateCount += 2U;
        model.SetTectonics(*terrainObject, tectonics);
        static_cast<void>(studio.Tick());

        const auto stale = studio.TerrainBake().Status(*terrainObject);
        Check(stale.has_value());
        Check(stale->state == terrain_bake::BakeState::Stale ||
              stale->state == terrain_bake::BakeState::Baking);
        Check(stale->currentRecipeHash != stale->activeRecipeHash);
        Check(SourceBake(studio, body) == first);

        // Hotspot edits are not baked and must not stale the bake. (Checked
        // after the rebake settles below.)

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        for (;;)
        {
            static_cast<void>(studio.Tick());
            const auto now = studio.TerrainBake().Status(*terrainObject);
            if (now.has_value() && now->state == terrain_bake::BakeState::Ready)
            {
                break;
            }
            Check(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        const auto after = SourceBake(studio, body);
        Check(after != nullptr && after != first);
        Check(after->RecipeHash() == studio.TerrainBake().Status(*terrainObject)->currentRecipeHash);

        // Hotspot edits change the terrain the rivers are baked on, but not the
        // tectonic rasters: only the river graph rebakes and the plate bake is
        // reused unchanged.
        auto hotspots = model.Tectonics(*terrainObject);
        hotspots.hotspotBaseReliefMeters += 100.0;
        model.SetTectonics(*terrainObject, hotspots);
        static_cast<void>(studio.Tick());
        const auto hotspotStatus = studio.TerrainBake().Status(*terrainObject);
        Check(hotspotStatus->activeRecipeHash == hotspotStatus->currentRecipeHash);
        Check(hotspotStatus->currentRiverHash != hotspotStatus->activeRiverHash);
        for (const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(90);;)
        {
            static_cast<void>(studio.Tick());
            const auto now = studio.TerrainBake().Status(*terrainObject);
            if (now.has_value() && now->state == terrain_bake::BakeState::Ready)
            {
                break;
            }
            Check(std::chrono::steady_clock::now() < deadline2);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        Check(SourceBake(studio, body) == after);
        Check(studio.TerrainBake().Status(*terrainObject)->activeRiverHash ==
              studio.TerrainBake().Status(*terrainObject)->currentRiverHash);
    }

    std::filesystem::remove_all(root);
    return 0;
}

        // The belt ridge relief is baked into the tectonic rasters, so editing it
        // persists and makes the plate bake stale.
        auto ridges = model.Tectonics(*terrainObject);
        Check(ridges.beltRidgeRelief == 1.0);
        ridges.beltRidgeRelief = 0.5;
        model.SetTectonics(*terrainObject, ridges);
        static_cast<void>(studio.Tick());
        Check(model.Tectonics(*terrainObject).beltRidgeRelief == 0.5);
        const auto ridgeStatus = studio.TerrainBake().Status(*terrainObject);
        Check(ridgeStatus->currentRecipeHash != ridgeStatus->activeRecipeHash);
        Check(Rpc(studio, "terrain.tectonics_get", "{\"terrain\":\"" + terrainObject->ToString() + "\"}")
                  .find("\"belt_ridge_relief\":0.5") != std::string::npos);
