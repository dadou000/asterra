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
#include <string>
#include <utility>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::abort();
    }
}

orbit::rpc::Value Call(
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

    orbit::rpc::Value document =
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
        ("orbit-editor-rpc-" +
         orbit::documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(root);

    auto project =
        orbit::documents::ProjectDocument::Create(
            root,
            "RPC Test");

    orbit::documents::WorldDatabase world(
        project.StartupWorldPath());

    orbit::schema::SchemaRegistry schemas;
    orbit::editor_model::builtin::
        RegisterSchemas(schemas);

    orbit::scene::ObjectStore objects(world);
    orbit::selection::SelectionService selection;
    orbit::commands::CommandService commandService(
        objects,
        schemas);
    orbit::commands::CommandRegistry commandRegistry;
    orbit::rpc::Dispatcher dispatcher;

    orbit::editor_rpc::EditorRpcService rpcService(
        dispatcher,
        project,
        commandRegistry,
        commandService,
        schemas,
        objects,
        selection);

    const auto projectInfo =
        Call(
            dispatcher,
            "1",
            "project.info");

    Check(
        projectInfo.Find("name") != nullptr &&
        projectInfo.Find("name")->AsString() ==
            "RPC Test");

    const auto schemaCatalog =
        Call(
            dispatcher,
            "2",
            "schema.catalog");

    Check(schemaCatalog.IsArray());
    Check(schemaCatalog.AsArray().size() >= 2);

    const auto created =
        Call(
            dispatcher,
            "3",
            "object.create",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {
                        "type",
                        orbit::editor_model::builtin::
                            kCelestialBodyType.
                            ToString()
                    },
                    {"name", "Asterra"}
                }));

    const auto* createdId =
        created.Find("id");

    Check(createdId != nullptr);
    const std::string objectId =
        createdId->AsString();

    const auto selected =
        Call(
            dispatcher,
            "4",
            "selection.set",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {
                        "ids",
                        orbit::rpc::Value::Array{
                            objectId
                        }
                    }
                }));

    Check(
        selected.Find("revision") != nullptr &&
        selected.Find("revision")->AsInteger() > 0);

    const auto selectionValue =
        Call(
            dispatcher,
            "5",
            "selection.get");

    Check(selectionValue.IsArray());
    Check(selectionValue.AsArray().size() == 1);
    Check(
        selectionValue.AsArray().
            front().
            AsString() ==
        objectId);

    static_cast<void>(
        Call(
            dispatcher,
            "6",
            "transaction.begin",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {"label", "RPC edit"}
                })));

    static_cast<void>(
        Call(
            dispatcher,
            "7",
            "object.rename",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {"id", objectId},
                    {"name", "Asterra Prime"}
                })));

    static_cast<void>(
        Call(
            dispatcher,
            "8",
            "property.set",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {"object", objectId},
                    {
                        "property",
                        orbit::editor_model::builtin::
                            kBodyRadius.
                            ToString()
                    },
                    {"value", 7'000'000.0}
                })));

    static_cast<void>(
        Call(
            dispatcher,
            "9",
            "transaction.commit"));

    const auto object =
        orbit::scene::ObjectId::Parse(
            objectId);

    Check(object.has_value());

    const auto renamed =
        objects.Find(*object);

    Check(renamed.has_value());
    Check(renamed->name == "Asterra Prime");

    const auto radius =
        objects.GetProperty(
            *object,
            orbit::editor_model::builtin::
                kBodyRadius);

    Check(radius.has_value());
    Check(
        std::get<orbit::f64>(*radius) ==
        7'000'000.0);

    static_cast<void>(
        Call(
            dispatcher,
            "10",
            "history.undo"));

    const auto undone =
        objects.Find(*object);

    Check(undone.has_value());
    Check(undone->name == "Asterra");
    Check(
        !objects.GetProperty(
            *object,
            orbit::editor_model::builtin::
                kBodyRadius).
            has_value());

    static_cast<void>(
        Call(
            dispatcher,
            "11",
            "history.redo"));

    const auto redone =
        objects.Find(*object);

    Check(redone.has_value());
    Check(redone->name == "Asterra Prime");

    editorRpc.PublishEvent(
        "test.changed",
        orbit::rpc::Value(
            orbit::rpc::Value::Object{
                {"value", 42}
            }));

    Check(
        editorRpc.LatestEventSequence() == 1);

    auto notifications =
        editorRpc.DrainNotifications();

    Check(notifications.size() == 1);

    const auto notification =
        orbit::rpc::ParseValue(
            notifications.front());

    Check(
        notification.Find("method") != nullptr &&
        notification.Find("method")->AsString() ==
            "event.test.changed");

    const auto replay =
        Call(
            dispatcher,
            "12",
            "event.since",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {"sequence", 0}
                }));

    Check(
        replay.Find("latest_sequence") != nullptr &&
        replay.Find("latest_sequence")->AsInteger() ==
            1);

    const auto* replayEvents =
        replay.Find("events");

    Check(replayEvents != nullptr);
    Check(replayEvents->IsArray());
    Check(replayEvents->AsArray().size() == 1);
    Check(
        replayEvents->AsArray()[0].
            Find("type")->AsString() ==
        "test.changed");

    const auto objectValue =
        Call(
            dispatcher,
            "13",
            "object.get",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {"id", objectId}
                }));

    const auto* properties =
        objectValue.Find("properties");

    Check(properties != nullptr);

    const auto* radiusValue =
        properties->Find(
            orbit::editor_model::builtin::
                kBodyRadius.
                ToString());

    Check(radiusValue != nullptr);
    Check(radiusValue->AsNumber() == 7'000'000.0);

    std::filesystem::remove_all(root);
    return 0;
}
