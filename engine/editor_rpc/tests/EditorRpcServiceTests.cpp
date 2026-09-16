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

#include <cassert>
#include <filesystem>
#include <string>

namespace
{
orbit::rpc::Value Call(
    orbit::rpc::Dispatcher& dispatcher,
    const std::string& id,
    const std::string& method,
    const std::string& params = "{}")
{
    const auto response =
        dispatcher.Dispatch(
            "{"jsonrpc":"2.0","id":"" +
            id +
            "","method":"" +
            method +
            "","params":" +
            params +
            "}");

    assert(response.has_value());

    auto document =
        orbit::rpc::ParseValue(
            *response);

    const auto* error =
        document.Find("error");

    assert(error == nullptr);

    const auto* result =
        document.Find("result");

    assert(result != nullptr);

    return *result;
}
}

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
    orbit::commands::CommandService commands(
        objects,
        schemas);
    orbit::commands::CommandRegistry registry;
    orbit::rpc::Dispatcher dispatcher;

    orbit::editor_rpc::EditorRpcService rpcService(
        dispatcher,
        project,
        registry,
        commands,
        schemas,
        objects,
        selection);

    const auto projectInfo =
        Call(
            dispatcher,
            "1",
            "project.info");

    assert(
        projectInfo.
            Find("name")->
            AsString() ==
        "RPC Test");

    const auto schemaCatalog =
        Call(
            dispatcher,
            "2",
            "schema.catalog");

    assert(schemaCatalog.IsArray());
    assert(
        schemaCatalog.AsArray().size() >= 2);

    const auto created =
        Call(
            dispatcher,
            "3",
            "object.create",
            "{"type":"" +
                orbit::editor_model::builtin::
                    kCelestialBodyType.
                    ToString() +
                "","name":"Asterra"}");

    const std::string objectId =
        created.Find("id")->AsString();

    const auto selected =
        Call(
            dispatcher,
            "4",
            "selection.set",
            "{"ids":["" +
                objectId +
                ""]}");

    assert(
        selected.Find("revision")->
            AsInteger() > 0);

    const auto selectionValue =
        Call(
            dispatcher,
            "5",
            "selection.get");

    assert(selectionValue.IsArray());
    assert(
        selectionValue.AsArray().
            front().
            AsString() ==
        objectId);

    static_cast<void>(
        Call(
            dispatcher,
            "6",
            "transaction.begin",
            "{"label":"RPC edit"}"));

    static_cast<void>(
        Call(
            dispatcher,
            "7",
            "object.rename",
            "{"id":"" +
                objectId +
                "","name":"Asterra Prime"}"));

    static_cast<void>(
        Call(
            dispatcher,
            "8",
            "property.set",
            "{"object":"" +
                objectId +
                "","property":"" +
                orbit::editor_model::builtin::
                    kBodyRadius.
                    ToString() +
                "","value":7000000.0}"));

    static_cast<void>(
        Call(
            dispatcher,
            "9",
            "transaction.commit"));

    const auto object =
        orbit::scene::ObjectId::Parse(
            objectId);

    assert(object.has_value());
    assert(
        objects.Find(*object)->name ==
        "Asterra Prime");
    assert(
        std::get<orbit::f64>(
            *objects.GetProperty(
                *object,
                orbit::editor_model::builtin::
                    kBodyRadius)) ==
        7'000'000.0);

    static_cast<void>(
        Call(
            dispatcher,
            "10",
            "history.undo"));

    assert(
        objects.Find(*object)->name ==
        "Asterra");
    assert(
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

    assert(
        objects.Find(*object)->name ==
        "Asterra Prime");

    const auto objectValue =
        Call(
            dispatcher,
            "12",
            "object.get",
            "{"id":"" +
                objectId +
                ""}");

    assert(
        objectValue.
            Find("properties")->
            Find(
                orbit::editor_model::builtin::
                    kBodyRadius.
                    ToString())->
            AsNumber() ==
        7'000'000.0);

    std::filesystem::remove_all(root);
    return 0;
}
