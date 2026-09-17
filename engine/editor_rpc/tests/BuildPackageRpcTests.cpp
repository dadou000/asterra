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
#include <optional>
#include <string>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Build package RPC test failed.\n";
        std::exit(1);
    }
}

orbit::rpc::Value Call(
    orbit::rpc::Dispatcher& dispatcher,
    const std::string& method)
{
    const orbit::rpc::Value request(
        orbit::rpc::Value::Object{
            {"jsonrpc", "2.0"},
            {"id", "package-test"},
            {"method", method},
            {
                "params",
                orbit::rpc::Value::Object{
                    {
                        "profile",
                        "Development Windows"
                    }
                }
            }
        });

    const auto response =
        dispatcher.Dispatch(
            orbit::rpc::Serialize(request));
    Check(response.has_value());

    const auto document =
        orbit::rpc::ParseValue(*response);
    Check(document.Find("error") == nullptr);

    const auto* result = document.Find("result");
    Check(result != nullptr);
    return *result;
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-build-package-rpc-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Build Package RPC Test");
        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());
        orbit::schema::SchemaRegistry schemas;
        orbit::editor_model::builtin::RegisterSchemas(schemas);
        orbit::scene::ObjectStore objects(world);
        orbit::selection::SelectionService selection;
        orbit::commands::CommandService commands(
            objects,
            schemas);
        orbit::commands::CommandRegistry registry;
        orbit::rpc::Dispatcher dispatcher;
        orbit::editor_rpc::EditorRpcService service(
            dispatcher,
            project,
            registry,
            commands,
            schemas,
            objects,
            selection);

        bool packaged = false;
        service.AttachBuild({
            .package =
                [&packaged](
                    std::optional<std::string> profile)
                {
                    packaged = true;
                    return orbit::rpc::Value(
                        orbit::rpc::Value::Object{
                            {"ok", true},
                            {
                                "profile",
                                profile.value_or(
                                    "Development Windows")
                            },
                            {
                                "executable",
                                "Build/RPC_Test.exe"
                            }
                        });
                }
        });

        const auto result = Call(
            dispatcher,
            "build.package");

        Check(packaged);
        Check(
            result.Find("executable") != nullptr &&
            result.Find("executable")->AsString() ==
                "Build/RPC_Test.exe");

        const auto notifications =
            service.DrainNotifications();
        Check(notifications.size() == 2U);

        const auto started =
            orbit::rpc::ParseValue(notifications[0]);
        const auto completed =
            orbit::rpc::ParseValue(notifications[1]);

        Check(
            started.Find("method") != nullptr &&
            started.Find("method")->AsString() ==
                "event.build.package_started");
        Check(
            completed.Find("method") != nullptr &&
            completed.Find("method")->AsString() ==
                "event.build.package_completed");
    }

    std::filesystem::remove_all(root);
    return 0;
}
