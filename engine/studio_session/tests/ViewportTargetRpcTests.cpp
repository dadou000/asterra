#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Viewport target RPC test failed.\n";
        std::exit(1);
    }
}

orbit::rpc::Value Call(
    orbit::studio_session::StudioSession& studio,
    const std::string& id,
    const std::string& method,
    orbit::rpc::Value params =
        orbit::rpc::Value(orbit::rpc::Value::Object{}))
{
    const orbit::rpc::Value request(
        orbit::rpc::Value::Object{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"method", method},
            {"params", std::move(params)}
        });

    const auto text =
        studio.DispatchRpc(
            orbit::rpc::Serialize(request));
    Check(text.has_value());

    const auto response =
        orbit::rpc::ParseValue(*text);
    Check(response.Find("error") == nullptr);
    Check(response.Find("result") != nullptr);
    return *response.Find("result");
}

orbit::scene::ObjectId AddBody(
    orbit::studio_session::StudioSession& studio,
    const orbit::scene::ObjectId parent,
    const std::string_view name,
    const double radius)
{
    auto& commands = studio.World().Commands();
    const auto body =
        commands.CreateObject(
            orbit::world_model::kCelestialBodyType,
            name,
            parent);
    commands.SetProperty(
        body,
        orbit::world_model::kBodyRadius,
        radius);
    commands.SetProperty(
        body,
        orbit::world_model::kBodyMass,
        5.0e24);
    return body;
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-viewport-rpc-" +
         orbit::documents::ProjectId::Random().ToString());
    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Viewport RPC Test");
        orbit::studio_session::StudioSession studio(project);
        auto& commands = studio.World().Commands();

        const auto world =
            commands.CreateObject(
                orbit::world_model::kWorldType,
                "World");
        const auto system =
            commands.CreateObject(
                orbit::world_model::kCelestialSystemType,
                "Helion",
                world);
        const auto asterra =
            AddBody(studio, system, "Asterra", 6'000'000.0);
        const auto luma =
            AddBody(studio, asterra, "Luma", 1'700'000.0);
        static_cast<void>(studio.Tick());

        const auto primary =
            Call(
                studio,
                "1",
                "view.register",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"id", "primary"},
                        {"mode", "perspective"},
                        {"follow_active_body", true}
                    }));
        Check(
            primary.Find("target") != nullptr &&
            primary.Find("target")->IsObject());
        Check(
            primary.Find("target")->
                Find("object")->AsString() ==
            asterra.ToString());
        Check(
            primary.Find("target")->
                Find("universe_generation")->AsInteger() ==
            static_cast<orbit::i64>(
                studio.World().UniverseGeneration()));

        static_cast<void>(
            Call(
                studio,
                "2",
                "view.register",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"id", "map"},
                        {"mode", "body_map"},
                        {"follow_active_body", false}
                    })));

        const auto pinned =
            Call(
                studio,
                "3",
                "view.pin",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"id", "map"},
                        {"object", luma.ToString()}
                    }));
        Check(
            pinned.Find("target")->
                Find("object")->AsString() ==
            luma.ToString());
        Check(
            pinned.Find("target")->
                Find("universe_generation")->AsInteger() ==
            static_cast<orbit::i64>(
                studio.World().UniverseGeneration()));
        Check(
            pinned.Find("mode")->AsString() ==
            "body_map");

        const std::array selected{luma};
        studio.World().Selection().Set(selected);
        static_cast<void>(studio.Tick());

        const auto catalog =
            Call(studio, "4", "view.list");
        Check(catalog.IsArray());
        Check(catalog.AsArray().size() == 2U);

        bool primaryTracksLuma = false;
        bool mapTracksLuma = false;

        for (const auto& view : catalog.AsArray())
        {
            const auto id = view.Find("id")->AsString();
            const auto object =
                view.Find("target")->
                    Find("object")->AsString();
            Check(
                view.Find("target")->
                    Find("universe_generation")->AsInteger() ==
                static_cast<orbit::i64>(
                    studio.World().UniverseGeneration()));

            if (id == "primary")
            {
                primaryTracksLuma = object == luma.ToString();
            }
            else if (id == "map")
            {
                mapTracksLuma = object == luma.ToString();
            }
        }

        Check(primaryTracksLuma);
        Check(mapTracksLuma);

        const auto previousUniverseGeneration =
            studio.World().UniverseGeneration();
        studio.World().Commands().SetProperty(
            luma,
            orbit::world_model::kBodyRadius,
            1'800'000.0);
        static_cast<void>(studio.Tick());
        Check(
            studio.World().UniverseGeneration() >
            previousUniverseGeneration);

        const auto refreshedCatalog =
            Call(studio, "4b", "view.list");
        for (const auto& view : refreshedCatalog.AsArray())
        {
            Check(
                view.Find("target")->
                    Find("universe_generation")->AsInteger() ==
                static_cast<orbit::i64>(
                    studio.World().UniverseGeneration()));
        }

        const auto debug =
            Call(
                studio,
                "5",
                "view.set_mode",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"id", "primary"},
                        {"mode", "debug"}
                    }));
        Check(debug.Find("mode")->AsString() == "debug");

        studio.CloseWorld();
        const auto closedCatalog =
            Call(studio, "6", "view.list");
        Check(closedCatalog.AsArray().size() == 2U);

        for (const auto& view : closedCatalog.AsArray())
        {
            Check(view.Find("target")->IsNull());
        }

        const auto removed =
            Call(
                studio,
                "7",
                "view.unregister",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"id", "map"}
                    }));
        Check(removed.Find("ok")->AsBool());
        Check(
            Call(studio, "8", "view.list").
                AsArray().size() == 1U);
    }

    std::filesystem::remove_all(root);
    return 0;
}
