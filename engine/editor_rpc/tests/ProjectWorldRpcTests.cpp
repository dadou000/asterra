#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>
#include <orbit/editor_rpc/EditorRpcService.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <string>
#include <utility>

namespace
{
void Check(
    const bool condition,
    const std::source_location location =
        std::source_location::current())
{
    if (!condition)
    {
        std::cerr
            << "Project world RPC test failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}

[[nodiscard]] orbit::rpc::Value Call(
    orbit::rpc::Dispatcher& dispatcher,
    std::string id,
    std::string method,
    orbit::rpc::Value params =
        orbit::rpc::Value(
            orbit::rpc::Value::Object{}))
{
    orbit::rpc::Value request(
        orbit::rpc::Value::Object{
            {"jsonrpc", "2.0"},
            {"id", std::move(id)},
            {"method", std::move(method)},
            {"params", std::move(params)}
        });

    const auto response =
        dispatcher.Dispatch(
            orbit::rpc::Serialize(request));
    Check(response.has_value());

    const auto document =
        orbit::rpc::ParseValue(*response);
    Check(document.Find("error") == nullptr);

    const auto* result =
        document.Find("result");
    Check(result != nullptr);
    return *result;
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-project-world-rpc-" +
         orbit::documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "World RPC Test");
        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());
        orbit::schema::SchemaRegistry schemas;
        orbit::editor_model::builtin::
            RegisterSchemas(schemas);
        orbit::scene::ObjectStore objects(world);
        orbit::selection::SelectionService selection;
        orbit::commands::CommandService commands(
            objects,
            schemas);
        orbit::commands::CommandRegistry registry;
        orbit::rpc::Dispatcher dispatcher;

        orbit::editor_rpc::EditorRpcService rpc(
            dispatcher,
            project,
            registry,
            commands,
            schemas,
            objects,
            selection);
        static_cast<void>(rpc);

        const auto initial =
            Call(
                dispatcher,
                "1",
                "world.list");
        Check(initial.IsArray());
        Check(initial.AsArray().size() == 1);
        Check(
            initial.AsArray()[0].
                Find("display_name")->AsString() ==
            "Main");
        Check(
            initial.AsArray()[0].
                Find("startup")->AsBool());

        const auto created =
            Call(
                dispatcher,
                "2",
                "world.create",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"path", "Secondary"},
                        {
                            "display_name",
                            "Secondary World"
                        }
                    }));
        Check(
            created.Find("path")->AsString() ==
            "Worlds/Secondary.orbitworld");
        Check(
            created.Find("display_name")->AsString() ==
            "Secondary World");
        Check(!created.Find("startup")->AsBool());
        Check(
            std::filesystem::is_regular_file(
                root /
                "Worlds/Secondary.orbitworld"));

        const auto described =
            Call(
                dispatcher,
                "3",
                "world.describe",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {
                            "path",
                            "Worlds/Secondary.orbitworld"
                        }
                    }));
        Check(
            described.Find("id")->AsString() ==
            created.Find("id")->AsString());

        const auto renamed =
            Call(
                dispatcher,
                "4",
                "world.set_display_name",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"path", "Secondary"},
                        {
                            "display_name",
                            "Secondary Renamed"
                        }
                    }));
        Check(
            renamed.Find("display_name")->AsString() ==
            "Secondary Renamed");
        Check(
            renamed.Find("id")->AsString() ==
            created.Find("id")->AsString());

        const auto startup =
            Call(
                dispatcher,
                "5",
                "world.set_startup",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"path", "Secondary"}
                    }));
        Check(startup.Find("startup")->AsBool());
        Check(
            project.Manifest().startupWorld ==
            std::filesystem::path(
                "Worlds/Secondary.orbitworld"));

        const auto catalog =
            Call(
                dispatcher,
                "6",
                "world.list");
        Check(catalog.IsArray());
        Check(catalog.AsArray().size() == 2);

        bool foundStartup = false;
        for (const auto& descriptor :
             catalog.AsArray())
        {
            if (descriptor.Find("startup")->AsBool())
            {
                Check(
                    descriptor.Find("path")->AsString() ==
                    "Worlds/Secondary.orbitworld");
                Check(
                    descriptor.Find("display_name")->AsString() ==
                    "Secondary Renamed");
                foundStartup = true;
            }
        }
        Check(foundStartup);

        const auto events =
            Call(
                dispatcher,
                "7",
                "event.since",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"sequence", orbit::i64{0}}
                    }));
        Check(events.Find("events") != nullptr);
        Check(events.Find("events")->IsArray());
        Check(
            events.Find("events")->AsArray().size() ==
            3);
    }

    std::filesystem::remove_all(root);
    return 0;
}
