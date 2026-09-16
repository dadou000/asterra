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
            << "EditorRpc test failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
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

    {

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

    const auto hiddenCommand =
        orbit::commands::CommandId::Random();
    const auto visibleCommand =
        orbit::commands::CommandId::Random();

    commandRegistry.Register({
        .id = hiddenCommand,
        .name = "Hidden Tool",
        .category = "Test",
        .description = "Must not appear in automation.",
        .automationVisible = false,
        .invoke =
            [](const orbit::commands::CommandArguments&)
            {
            }
    });

    commandRegistry.Register({
        .id = visibleCommand,
        .name = "Visible Tool",
        .category = "Test",
        .description = "Automation-visible test command.",
        .automationVisible = true,
        .invoke =
            [](const orbit::commands::CommandArguments&)
            {
            }
    });

    orbit::rpc::Dispatcher dispatcher;

    orbit::editor_rpc::EditorRpcService rpcService(
        dispatcher,
        project,
        commandRegistry,
        commandService,
        schemas,
        objects,
        selection);
    auto& editorRpc = rpcService;

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

    const auto commandCatalog =
        Call(
            dispatcher,
            "2b",
            "command.catalog");

    Check(commandCatalog.IsArray());
    Check(commandCatalog.AsArray().size() == 1);
    Check(
        commandCatalog.AsArray()[0].
            Find("id")->AsString() ==
        visibleCommand.ToString());

    static_cast<void>(
        Call(
            dispatcher,
            "2c",
            "command.invoke",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {
                        "id",
                        visibleCommand.ToString()
                    },
                    {
                        "arguments",
                        orbit::rpc::Value::Object{}
                    }
                })));

    const orbit::u64 revisionBeforeCreate =
        objects.Revision();

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

    Check(
        objects.Revision() ==
        revisionBeforeCreate + 1U);

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

    const orbit::u64
        revisionBeforeTransaction =
            objects.Revision();

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

    Check(
        objects.Revision() ==
        revisionBeforeTransaction);

    static_cast<void>(
        Call(
            dispatcher,
            "9",
            "transaction.commit"));

    Check(
        objects.Revision() ==
        revisionBeforeTransaction + 1U);

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

    const orbit::u64
        revisionBeforeUndo =
            objects.Revision();

    static_cast<void>(
        Call(
            dispatcher,
            "10",
            "history.undo"));

    Check(
        objects.Revision() ==
        revisionBeforeUndo + 1U);

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

    const orbit::u64
        revisionBeforeRedo =
            objects.Revision();

    static_cast<void>(
        Call(
            dispatcher,
            "11",
            "history.redo"));

    Check(
        objects.Revision() ==
        revisionBeforeRedo + 1U);

    const auto redone =
        objects.Find(*object);

    Check(redone.has_value());
    Check(redone->name == "Asterra Prime");

    rpcService.PublishEvent(
        "test.changed",
        orbit::rpc::Value(
            orbit::rpc::Value::Object{
                {"value", static_cast<orbit::i64>(42)}
            }));

    Check(
        rpcService.LatestEventSequence() == 1);

    auto notifications =
        rpcService.DrainNotifications();

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
                    {"sequence", static_cast<orbit::i64>(0)}
                }));

    Check(
        replay.Find("oldest_sequence") != nullptr &&
        replay.Find("oldest_sequence")->AsInteger() ==
            1);
    Check(
        replay.Find("latest_sequence") != nullptr &&
        replay.Find("latest_sequence")->AsInteger() ==
            1);
    Check(
        replay.Find("truncated") != nullptr &&
        !replay.Find("truncated")->AsBool());

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

    const auto pathFrame =
        orbit::frames::FrameId::Random();

    static_cast<void>(
        Call(
            dispatcher,
            "14",
            "transaction.begin",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {
                        "label",
                        "RPC path edit"
                    }
                })));

    const auto pathNetwork =
        Call(
            dispatcher,
            "15",
            "path.create_network",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {"name", "RPC Roads"},
                    {
                        "profile_asset",
                        "11111111-1111-4111-8111-111111111111"
                    }
                }));

    const std::string pathNetworkId =
        pathNetwork.Find("id")->AsString();

    const auto pathNodeA =
        Call(
            dispatcher,
            "16",
            "path.create_node",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {
                        "network",
                        pathNetworkId
                    },
                    {"name", "Node A"},
                    {
                        "anchor",
                        orbit::rpc::Value::Object{
                            {"kind", "frame"},
                            {
                                "frame",
                                pathFrame.ToString()
                            },
                            {
                                "position",
                                orbit::rpc::Value::Array{
                                    0.0,
                                    0.0,
                                    0.0
                                }
                            }
                        }
                    }
                }));

    const auto pathNodeB =
        Call(
            dispatcher,
            "17",
            "path.create_node",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {
                        "network",
                        pathNetworkId
                    },
                    {"name", "Node B"},
                    {
                        "anchor",
                        orbit::rpc::Value::Object{
                            {"kind", "frame"},
                            {
                                "frame",
                                pathFrame.ToString()
                            },
                            {
                                "position",
                                orbit::rpc::Value::Array{
                                    100.0,
                                    0.0,
                                    0.0
                                }
                            }
                        }
                    }
                }));

    const std::string pathNodeAId =
        pathNodeA.Find("id")->AsString();
    const std::string pathNodeBId =
        pathNodeB.Find("id")->AsString();

    const auto pathEdge =
        Call(
            dispatcher,
            "18",
            "path.connect",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {"start", pathNodeAId},
                    {"end", pathNodeBId},
                    {"mode", "bezier"},
                    {
                        "start_handle",
                        orbit::rpc::Value::Array{
                            20.0,
                            5.0,
                            0.0
                        }
                    },
                    {
                        "end_handle",
                        orbit::rpc::Value::Array{
                            -20.0,
                            5.0,
                            0.0
                        }
                    }
                }));

    const std::string pathEdgeId =
        pathEdge.Find("id")->AsString();

    static_cast<void>(
        Call(
            dispatcher,
            "18p",
            "path.set_profile",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {"object", pathEdgeId},
                    {
                        "profile_asset",
                        "33333333-3333-4333-8333-333333333333"
                    }
                })));

    const auto inspectedPath =
        Call(
            dispatcher,
            "19",
            "path.inspect",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {"id", pathEdgeId}
                }));

    Check(
        inspectedPath.Find("mode") !=
            nullptr &&
        inspectedPath.Find("mode")->
            AsString() ==
            "bezier");
    Check(
        inspectedPath.Find(
            "profile_override") !=
            nullptr &&
        inspectedPath.Find(
            "profile_override")->
            AsString() ==
            "33333333-3333-4333-8333-333333333333");

    static_cast<void>(
        Call(
            dispatcher,
            "20",
            "path.set_bezier_handles",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {"edge", pathEdgeId},
                    {
                        "start_handle",
                        orbit::rpc::Value::Array{
                            30.0,
                            8.0,
                            0.0
                        }
                    },
                    {
                        "end_handle",
                        orbit::rpc::Value::Array{
                            -30.0,
                            8.0,
                            0.0
                        }
                    }
                })));

    static_cast<void>(
        Call(
            dispatcher,
            "21",
            "transaction.commit"));

    const auto pathNetworkObject =
        orbit::scene::ObjectId::Parse(
            pathNetwork.Find("object")->
                AsString());
    const auto pathNodeAObject =
        orbit::scene::ObjectId::Parse(
            pathNodeAId);
    const auto pathNodeBObject =
        orbit::scene::ObjectId::Parse(
            pathNodeBId);
    const auto pathEdgeObject =
        orbit::scene::ObjectId::Parse(
            pathEdgeId);

    Check(pathNetworkObject.has_value());
    Check(pathNodeAObject.has_value());
    Check(pathNodeBObject.has_value());
    Check(pathEdgeObject.has_value());
    Check(
        objects.Find(*pathNetworkObject).
            has_value());
    Check(
        objects.Find(*pathNodeAObject).
            has_value());
    Check(
        objects.Find(*pathNodeBObject).
            has_value());
    Check(
        objects.Find(*pathEdgeObject).
            has_value());

    bool routeInvalidated = false;

    editorRpc.AttachPathRouting({
        .status =
            [](const orbit::scene::ObjectId edge)
            {
                return orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {
                            "edge",
                            edge.ToString()
                        },
                        {"state", "ready"},
                        {"generation", 4}
                    });
            },
        .result =
            [](const orbit::scene::ObjectId edge)
            {
                return orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {
                            "edge",
                            edge.ToString()
                        },
                        {"ready", true},
                        {
                            "points",
                            orbit::rpc::Value::Array{
                                orbit::rpc::Value(
                                    orbit::rpc::Value::Object{
                                        {
                                            "position",
                                            orbit::rpc::Value::Array{
                                                0.0,
                                                0.0,
                                                0.0
                                            }
                                        }
                                    }),
                                orbit::rpc::Value(
                                    orbit::rpc::Value::Object{
                                        {
                                            "position",
                                            orbit::rpc::Value::Array{
                                                1.0,
                                                0.0,
                                                0.0
                                            }
                                        }
                                    })
                            }
                        }
                    });
            },
        .invalidate =
            [&routeInvalidated](
                const orbit::scene::ObjectId)
            {
                routeInvalidated = true;
            }
    });

    const auto routeStatus =
        Call(
            dispatcher,
            "21r1",
            "path.route_status",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {"edge", pathEdgeId}
                }));

    Check(
        routeStatus.Find("state") !=
            nullptr &&
        routeStatus.Find("state")->
            AsString() ==
            "ready");

    const auto routeResult =
        Call(
            dispatcher,
            "21r2",
            "path.route_result",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {"edge", pathEdgeId}
                }));

    Check(
        routeResult.Find("ready") !=
            nullptr &&
        routeResult.Find("ready")->
            AsBool());
    Check(
        routeResult.Find("points") !=
            nullptr &&
        routeResult.Find("points")->
            AsArray().size() ==
            2);

    static_cast<void>(
        Call(
            dispatcher,
            "21r3",
            "path.route_invalidate",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {"edge", pathEdgeId}
                })));

    Check(routeInvalidated);

    editorRpc.AttachPathGeometry({
        .result =
            [](const orbit::scene::ObjectId edge)
            {
                return orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {
                            "edge",
                            edge.ToString()
                        },
                        {"ready", true},
                        {"lane_count", 2},
                        {"visual_vertices", 12},
                        {"collision_triangles", 10},
                        {"reference_nodes", 6}
                    });
            }
    });

    const auto derived =
        Call(
            dispatcher,
            "21g1",
            "path.derived_result",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {"edge", pathEdgeId}
                }));

    Check(
        derived.Find("ready") !=
            nullptr &&
        derived.Find("ready")->AsBool());
    Check(
        derived.Find("lane_count") !=
            nullptr &&
        derived.Find("lane_count")->
            AsInteger() == 2);
    Check(
        derived.Find("collision_triangles") !=
            nullptr &&
        derived.Find("collision_triangles")->
            AsInteger() == 10);

    bool buildCooked = false;

    editorRpc.AttachBuild({
        .profiles =
            []
            {
                return orbit::rpc::Value(
                    orbit::rpc::Value::Array{
                        orbit::rpc::Value(
                            orbit::rpc::Value::Object{
                                {
                                    "name",
                                    "Development Windows"
                                },
                                {
                                    "configuration",
                                    "Development"
                                },
                                {
                                    "platform",
                                    "Windows"
                                },
                                {
                                    "storefront",
                                    "standalone"
                                }
                            })
                    });
            },
        .validate =
            [](
                std::optional<std::string>
                    profile)
            {
                return orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"ok", true},
                        {
                            "profile",
                            profile.value_or(
                                "Development Windows")
                        }
                    });
            },
        .cook =
            [&buildCooked](
                std::optional<std::string>
                    profile)
            {
                buildCooked = true;

                return orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"ok", true},
                        {
                            "profile",
                            profile.value_or(
                                "Development Windows")
                        },
                        {
                            "manifest",
                            "Build/OrbitBuildManifest.toml"
                        }
                    });
            }
    });

    const auto buildProfiles =
        Call(
            dispatcher,
            "21b1",
            "build.profiles");

    Check(
        buildProfiles.IsArray() &&
        buildProfiles.AsArray().size() ==
            1U);

    const auto buildValidation =
        Call(
            dispatcher,
            "21b2",
            "build.validate",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {
                        "profile",
                        "Development Windows"
                    }
                }));

    Check(
        buildValidation.Find("ok") !=
            nullptr &&
        buildValidation.Find("ok")->
            AsBool());

    const auto buildCook =
        Call(
            dispatcher,
            "21b3",
            "build.cook",
            orbit::rpc::Value(
                orbit::rpc::Value::Object{
                    {
                        "profile",
                        "Development Windows"
                    }
                }));

    Check(buildCooked);
    Check(
        buildCook.Find("manifest") !=
            nullptr &&
        buildCook.Find("manifest")->
            AsString() ==
            "Build/OrbitBuildManifest.toml");

    const auto buildNotifications =
        editorRpc.DrainNotifications();

    Check(
        !buildNotifications.empty());

    const auto buildNotification =
        orbit::rpc::ParseValue(
            buildNotifications.back());

    Check(
        buildNotification.Find("method") !=
            nullptr &&
        buildNotification.Find("method")->
            AsString() ==
            "event.build.cooked");

    static_cast<void>(
        Call(
            dispatcher,
            "22",
            "history.undo"));

    Check(
        !objects.Find(*pathNetworkObject).
            has_value());
    Check(
        !objects.Find(*pathNodeAObject).
            has_value());
    Check(
        !objects.Find(*pathNodeBObject).
            has_value());
    Check(
        !objects.Find(*pathEdgeObject).
            has_value());

    }

    std::filesystem::remove_all(root);
    return 0;
}
