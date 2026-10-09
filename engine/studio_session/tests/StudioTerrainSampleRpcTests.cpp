#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
using namespace orbit;

void CheckImpl(const bool condition, const char* expression, const int line)
{
    if (!condition)
    {
        std::cerr << "Studio terrain sample RPC test failed at line " << line << ": "
                  << expression << '\n';
        std::exit(1);
    }
}

#define Check(condition) CheckImpl((condition), #condition, __LINE__)

std::string Rpc(
    studio_session::StudioSession& studio,
    const std::string& method,
    const std::string& params)
{
    const auto reply = studio.DispatchRpc(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"" + method + "\",\"params\":" + params + "}");
    Check(reply.has_value());
    return *reply;
}

bool Has(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

std::size_t Count(const std::string& text, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1U))
        ++count;
    return count;
}
} // namespace

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("orbit-studio-terrain-sample-" + documents::ProjectId::Random().ToString());
    std::filesystem::remove_all(root);

    {
        auto project = documents::ProjectDocument::Create(root, "Studio Terrain Sample Test");
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
        static_cast<void>(studio.Tick());

        // Process identity: a client polls this to notice a relaunch.
        const auto info = Rpc(studio, "studio.process_info", "{}");
        Check(Has(info, "\"pid\":"));
        Check(Has(info, "\"terrain_count\":1"));

        const auto list = Rpc(studio, "terrain.list", "{}");
        Check(Has(list, "\"terrains\":["));
        Check(Has(list, "\"composed\":true"));
        Check(Has(list, "\"planet_radius_meters\":"));
        Check(Has(list, "\"sea_level_meters\":"));

        const auto body = studio.ActiveBody().Active()->body;
        const auto terrainObject = studio.World().Surfaces().TerrainObjectForBody(body);
        Check(terrainObject.has_value());
        const std::string terrain = "\"terrain\":\"" + terrainObject->ToString() + "\"";
        Check(Has(list, terrainObject->ToString()));

        // Points: one sample per point, in order, with the full field set.
        const auto points = Rpc(
            studio, "terrain.sample",
            "{" + terrain + ",\"points\":[{\"latitude_degrees\":10,\"longitude_degrees\":20},"
            "{\"latitude_degrees\":-45,\"longitude_degrees\":170}]}");
        Check(!Has(points, "\"error\""));
        Check(Count(points, "\"elevation_meters\":") == 2U);
        Check(Has(points, "\"biome_weights\":"));
        Check(Has(points, "\"underwater\":"));
        Check(!Has(points, "\"tectonics\":"));

        // Transect: count samples along a great circle, endpoints included.
        const auto transect = Rpc(
            studio, "terrain.sample",
            "{" + terrain + ",\"include_tectonics\":true,\"transect\":{"
            "\"from\":{\"latitude_degrees\":40,\"longitude_degrees\":-30},"
            "\"to\":{\"latitude_degrees\":55,\"longitude_degrees\":-10},\"count\":9}}");
        Check(!Has(transect, "\"error\""));
        Check(Count(transect, "\"elevation_meters\":") == 9U);
        Check(Count(transect, "\"tectonics\":") == 9U);
        Check(Has(transect, "\"structural_elevation_meters\":"));
        Check(Has(transect, "\"fracture_density\":"));

        // The same point sampled twice agrees (the CPU path is deterministic).
        const auto again = Rpc(
            studio, "terrain.sample",
            "{" + terrain + ",\"points\":[{\"latitude_degrees\":10,\"longitude_degrees\":20}]}");
        const auto first = points.substr(points.find("\"elevation_meters\":"), 40U);
        Check(again.find(first) != std::string::npos);

        // Bad input is a parameter error, never a crash.
        Check(Has(Rpc(studio, "terrain.sample", "{" + terrain + "}"), "\"error\""));
        Check(Has(Rpc(studio, "terrain.sample",
                      "{" + terrain + ",\"points\":[],\"transect\":{}}"), "\"error\""));
        Check(Has(Rpc(studio, "terrain.sample",
                      "{" + terrain + ",\"points\":[{\"latitude_degrees\":95,\"longitude_degrees\":0}]}"),
                  "\"error\""));
        Check(Has(Rpc(studio, "terrain.sample",
                      "{" + terrain + ",\"transect\":{\"from\":{\"latitude_degrees\":0,"
                      "\"longitude_degrees\":0},\"to\":{\"latitude_degrees\":1,\"longitude_degrees\":1},"
                      "\"count\":1}}"), "\"error\""));
        Check(Has(Rpc(studio, "terrain.sample",
                      "{\"terrain\":\"not-an-id\",\"points\":[{\"latitude_degrees\":0,\"longitude_degrees\":0}]}"),
                  "\"error\""));
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return 0;
}
