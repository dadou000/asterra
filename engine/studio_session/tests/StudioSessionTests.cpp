#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Studio session test failed.\n";
        std::exit(1);
    }
}

[[nodiscard]] orbit::rpc::Value RpcCall(
    orbit::studio_session::StudioSession& studio,
    const std::string& id,
    const std::string& method,
    orbit::rpc::Value params =
        orbit::rpc::Value(
            orbit::rpc::Value::Object{}))
{
    const orbit::rpc::Value request(
        orbit::rpc::Value::Object{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"method", method},
            {"params", std::move(params)}
        });

    const auto responseText =
        studio.DispatchRpc(
            orbit::rpc::Serialize(request));
    Check(responseText.has_value());

    const auto response =
        orbit::rpc::ParseValue(*responseText);
    Check(response.Find("error") == nullptr);
    Check(response.Find("result") != nullptr);
    return *response.Find("result");
}

orbit::scene::ObjectId AddBody(
    orbit::studio_session::StudioSession& studio,
    const std::string& name,
    const double radius)
{
    auto& world = studio.World();
    auto roots = world.Objects().Roots();

    orbit::scene::ObjectId worldObject{};
    if (roots.empty())
    {
        worldObject =
            world.Commands().CreateObject(
                orbit::world_model::kWorldType,
                "World");
    }
    else
    {
        worldObject = roots.front().id;
    }

    const auto system =
        world.Commands().CreateObject(
            orbit::world_model::kCelestialSystemType,
            "Helion",
            worldObject);
    const auto body =
        world.Commands().CreateObject(
            orbit::world_model::kCelestialBodyType,
            name,
            system);

    world.Commands().SetProperty(
        body,
        orbit::world_model::kBodyRadius,
        radius);
    world.Commands().SetProperty(
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
        ("orbit-studio-session-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Studio Session Test");
        orbit::studio_session::StudioSession studio(project);

        const auto asterra =
            AddBody(
                studio,
                "Asterra",
                6'000'000.0);
        static_cast<void>(studio.Tick());
        Check(studio.ActiveBody().Active().has_value());
        Check(
            studio.ActiveBody().Active()->semanticObject ==
            asterra);

        const auto secondary =
            studio.CreateWorld(
                "Secondary",
                "Secondary");
        Check(studio.Worlds().size() == 2U);

        studio.OpenWorld(secondary.relativePath);
        Check(studio.ActiveWorld().has_value());
        Check(
            studio.ActiveWorld()->descriptor.id ==
            secondary.id);
        Check(studio.World().Explorer().Roots().empty());
        Check(!studio.ActiveBody().Active().has_value());

        const auto veyra =
            AddBody(
                studio,
                "Veyra",
                4'200'000.0);
        static_cast<void>(studio.Tick());
        Check(studio.ActiveBody().Active().has_value());
        Check(
            studio.ActiveBody().Active()->semanticObject ==
            veyra);

        const auto roots =
            RpcCall(
                studio,
                "1",
                "object.roots");
        Check(roots.IsArray());
        Check(roots.AsArray().size() == 1U);
        Check(
            roots.AsArray().front().
                Find("name")->AsString() ==
            "World");

        studio.CloseWorld();
        Check(!studio.World().HasWorld());
        Check(!studio.ActiveWorld().has_value());
        Check(!studio.ActiveBody().Active().has_value());

        const auto catalog =
            RpcCall(
                studio,
                "2",
                "world.list");
        Check(catalog.IsArray());
        Check(catalog.AsArray().size() == 2U);

        studio.OpenWorld("Main");
        static_cast<void>(studio.Tick());
        Check(studio.World().HasWorld());
        Check(studio.ActiveBody().Active().has_value());
        Check(
            studio.ActiveBody().Active()->semanticObject ==
            asterra);
        Check(
            studio.World().Explorer().Roots().size() ==
            1U);
    }

    std::filesystem::remove_all(root);
    return 0;
}
