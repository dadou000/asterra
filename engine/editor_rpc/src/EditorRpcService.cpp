#include <orbit/editor_rpc/EditorRpcService.hpp>

#include <orbit/math/Vector.hpp>
#include <orbit/paths/PathNetwork.hpp>

#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace orbit::editor_rpc
{
namespace
{
[[nodiscard]] const rpc::Value::Object&
RequireObject(
    const rpc::Value& params)
{
    if (!params.IsObject())
    {
        throw rpc::Error(
            -32602,
            "Params must be an object.");
    }

    return params.AsObject();
}

[[nodiscard]] const rpc::Value&
Require(
    const rpc::Value::Object& object,
    const std::string_view key)
{
    const auto found =
        object.find(key);

    if (found == object.end())
    {
        throw rpc::Error(
            -32602,
            "Missing parameter: " +
                std::string(key));
    }

    return found->second;
}

[[nodiscard]] std::optional<
    scene::ObjectId>
OptionalObjectId(
    const rpc::Value::Object& object,
    const std::string_view key)
{
    const auto found =
        object.find(key);

    if (found == object.end() ||
        found->second.IsNull())
    {
        return std::nullopt;
    }

    if (!found->second.IsString())
    {
        throw rpc::Error(
            -32602,
            "Object ID parameter must be a string.");
    }

    const auto id =
        scene::ObjectId::Parse(
            found->second.AsString());

    if (!id.has_value())
    {
        throw rpc::Error(
            -32602,
            "Invalid object ID.");
    }

    return *id;
}

[[nodiscard]] scene::ObjectId
RequireObjectId(
    const rpc::Value::Object& object,
    const std::string_view key)
{
    const auto id =
        OptionalObjectId(
            object,
            key);

    if (!id.has_value())
    {
        throw rpc::Error(
            -32602,
            "Missing object ID: " +
                std::string(key));
    }

    return *id;
}

[[nodiscard]] schema::TypeId
RequireTypeId(
    const rpc::Value::Object& object,
    const std::string_view key)
{
    const rpc::Value& value =
        Require(object, key);

    if (!value.IsString())
    {
        throw rpc::Error(
            -32602,
            "Type ID parameter must be a string.");
    }

    const auto id =
        schema::TypeId::Parse(
            value.AsString());

    if (!id.has_value())
    {
        throw rpc::Error(
            -32602,
            "Invalid type ID.");
    }

    return *id;
}

[[nodiscard]] schema::PropertyId
RequirePropertyId(
    const rpc::Value::Object& object,
    const std::string_view key)
{
    const rpc::Value& value =
        Require(object, key);

    if (!value.IsString())
    {
        throw rpc::Error(
            -32602,
            "Property ID parameter must be a string.");
    }

    const auto id =
        schema::PropertyId::Parse(
            value.AsString());

    if (!id.has_value())
    {
        throw rpc::Error(
            -32602,
            "Invalid property ID.");
    }

    return *id;
}

[[nodiscard]] const char*
PropertyKindName(
    const schema::PropertyKind kind) noexcept
{
    switch (kind)
    {
    case schema::PropertyKind::Boolean:
        return "boolean";
    case schema::PropertyKind::Integer:
        return "integer";
    case schema::PropertyKind::Float:
        return "float";
    case schema::PropertyKind::String:
        return "string";
    case schema::PropertyKind::Vector3:
        return "vector3";
    case schema::PropertyKind::ObjectReference:
        return "object_id";
    }

    return "unknown";
}

[[nodiscard]] const char*
CommandKindName(
    const commands::CommandValueKind kind) noexcept
{
    switch (kind)
    {
    case commands::CommandValueKind::Boolean:
        return "boolean";
    case commands::CommandValueKind::Integer:
        return "integer";
    case commands::CommandValueKind::Float:
        return "float";
    case commands::CommandValueKind::String:
        return "string";
    case commands::CommandValueKind::Vector3:
        return "vector3";
    case commands::CommandValueKind::ObjectId:
        return "object_id";
    case commands::CommandValueKind::PropertyId:
        return "property_id";
    }

    return "unknown";
}

[[nodiscard]] rpc::Value
PropertyValueToRpc(
    const schema::PropertyValue& value)
{
    return std::visit(
        [](const auto& item)
            -> rpc::Value
        {
            using Value =
                std::decay_t<
                    decltype(item)>;

            if constexpr (
                std::is_same_v<Value, bool> ||
                std::is_same_v<Value, i64> ||
                std::is_same_v<Value, f64> ||
                std::is_same_v<Value, std::string>)
            {
                return rpc::Value(item);
            }
            else if constexpr (
                std::is_same_v<
                    Value,
                    math::Double3>)
            {
                return rpc::Value(
                    rpc::Value::Array{
                        item.x,
                        item.y,
                        item.z
                    });
            }
            else
            {
                const scene::ObjectId id{
                    .high = item.high,
                    .low = item.low
                };

                return rpc::Value(
                    id.IsValid()
                        ? id.ToString()
                        : std::string{});
            }
        },
        value);
}

[[nodiscard]] schema::PropertyValue
RpcToPropertyValue(
    const schema::PropertySchema& property,
    const rpc::Value& value)
{
    try
    {
        switch (property.kind)
        {
        case schema::PropertyKind::Boolean:
            return value.AsBool();

        case schema::PropertyKind::Integer:
            return value.AsInteger();

        case schema::PropertyKind::Float:
            return value.AsNumber();

        case schema::PropertyKind::String:
            return value.AsString();

        case schema::PropertyKind::Vector3:
        {
            const auto& array =
                value.AsArray();

            if (array.size() != 3)
            {
                throw std::bad_variant_access{};
            }

            return math::Double3{
                array[0].AsNumber(),
                array[1].AsNumber(),
                array[2].AsNumber()
            };
        }

        case schema::PropertyKind::ObjectReference:
        {
            const auto id =
                scene::ObjectId::Parse(
                    value.AsString());

            if (!id.has_value())
            {
                throw rpc::Error(
                    -32602,
                    "Invalid object reference.");
            }

            return schema::ObjectReferenceValue{
                .high = id->high,
                .low = id->low
            };
        }
        }
    }
    catch (const std::bad_variant_access&)
    {
        throw rpc::Error(
            -32602,
            "Property value has the wrong JSON type.");
    }

    throw rpc::Error(
        -32602,
        "Unsupported property kind.");
}

[[nodiscard]] commands::CommandValue
RpcToCommandValue(
    const commands::CommandValueKind kind,
    const rpc::Value& value)
{
    try
    {
        switch (kind)
        {
        case commands::CommandValueKind::Boolean:
            return value.AsBool();

        case commands::CommandValueKind::Integer:
            return value.AsInteger();

        case commands::CommandValueKind::Float:
            return value.AsNumber();

        case commands::CommandValueKind::String:
            return value.AsString();

        case commands::CommandValueKind::Vector3:
        {
            const auto& array =
                value.AsArray();

            if (array.size() != 3)
            {
                throw std::bad_variant_access{};
            }

            return math::Double3{
                array[0].AsNumber(),
                array[1].AsNumber(),
                array[2].AsNumber()
            };
        }

        case commands::CommandValueKind::ObjectId:
        {
            const auto id =
                scene::ObjectId::Parse(
                    value.AsString());

            if (!id.has_value())
            {
                throw rpc::Error(
                    -32602,
                    "Invalid command object ID.");
            }

            return *id;
        }

        case commands::CommandValueKind::PropertyId:
        {
            const auto id =
                schema::PropertyId::Parse(
                    value.AsString());

            if (!id.has_value())
            {
                throw rpc::Error(
                    -32602,
                    "Invalid command property ID.");
            }

            return *id;
        }
        }
    }
    catch (const std::bad_variant_access&)
    {
        throw rpc::Error(
            -32602,
            "Command argument has the wrong JSON type.");
    }

    throw rpc::Error(
        -32602,
        "Unsupported command argument kind.");
}

[[nodiscard]] rpc::Value
ObjectToRpc(
    const scene::ObjectRecord& object)
{
    rpc::Value::Object result{
        {"id", object.id.ToString()},
        {"type", object.type.ToString()},
        {"name", object.name},
        {"sort_order", object.sortOrder}
    };

    result.emplace(
        "parent",
        object.parent.has_value()
            ? rpc::Value(
                  object.parent->
                      ToString())
            : rpc::Value{});

    return rpc::Value(
        std::move(result));
}

[[nodiscard]] rpc::Value
ViewportToRpc(
    const render_view::RenderView& view)
{
    const auto& camera =
        view.Camera();

    rpc::Value::Object result{
        {
            "width",
            static_cast<i64>(
                view.Width())
        },
        {
            "height",
            static_cast<i64>(
                view.Height())
        },
        {
            "position",
            rpc::Value::Array{
                camera.localPositionMeters.x,
                camera.localPositionMeters.y,
                camera.localPositionMeters.z
            }
        },
        {
            "forward",
            rpc::Value::Array{
                static_cast<f64>(
                    camera.forward.x),
                static_cast<f64>(
                    camera.forward.y),
                static_cast<f64>(
                    camera.forward.z)
            }
        },
        {
            "up",
            rpc::Value::Array{
                static_cast<f64>(
                    camera.up.x),
                static_cast<f64>(
                    camera.up.y),
                static_cast<f64>(
                    camera.up.z)
            }
        },
        {
            "vertical_fov_radians",
            static_cast<f64>(
                camera.verticalFovRadians)
        },
        {
            "near_plane_meters",
            static_cast<f64>(
                camera.nearPlaneMeters)
        },
        {
            "far_plane_meters",
            static_cast<f64>(
                camera.farPlaneMeters)
        }
    };

    result.emplace(
        "frame",
        camera.frame
            ? rpc::Value(
                  camera.frame.ToString())
            : rpc::Value{});

    return rpc::Value(
        std::move(result));
}

[[nodiscard]] f64
RequireNumber(
    const rpc::Value& value,
    const std::string_view name)
{
    try
    {
        return value.AsNumber();
    }
    catch (const std::bad_variant_access&)
    {
        throw rpc::Error(
            -32602,
            std::string(name) +
                " must be a number.");
    }
}

[[nodiscard]] math::Double3
RequireDouble3(
    const rpc::Value& value,
    const std::string_view name)
{
    if (!value.IsArray() ||
        value.AsArray().size() != 3)
    {
        throw rpc::Error(
            -32602,
            std::string(name) +
                " must be a 3-number array.");
    }

    try
    {
        return {
            value.AsArray()[0].AsNumber(),
            value.AsArray()[1].AsNumber(),
            value.AsArray()[2].AsNumber()
        };
    }
    catch (const std::bad_variant_access&)
    {
        throw rpc::Error(
            -32602,
            std::string(name) +
                " must contain numbers.");
    }
}

[[nodiscard]] std::string
RequireString(
    const rpc::Value::Object& object,
    const std::string_view key)
{
    const rpc::Value& value =
        Require(object, key);

    if (!value.IsString() ||
        value.AsString().empty())
    {
        throw rpc::Error(
            -32602,
            std::string(key) +
                " must be a non-empty string.");
    }

    return value.AsString();
}

[[nodiscard]] std::optional<std::string>
OptionalString(
    const rpc::Value::Object& object,
    const std::string_view key)
{
    const auto found =
        object.find(key);

    if (found == object.end() ||
        found->second.IsNull())
    {
        return std::nullopt;
    }

    if (!found->second.IsString())
    {
        throw rpc::Error(
            -32602,
            std::string(key) +
                " must be a string.");
    }

    return found->second.AsString();
}

[[nodiscard]] paths::NetworkId
RequireNetworkId(
    const rpc::Value::Object& object,
    const std::string_view key)
{
    const auto parsed =
        paths::NetworkId::Parse(
            RequireString(
                object,
                key));

    if (!parsed.has_value())
    {
        throw rpc::Error(
            -32602,
            "Invalid path network ID.");
    }

    return *parsed;
}

[[nodiscard]] paths::PathAnchor
PathAnchorFromRpc(
    const rpc::Value& value)
{
    if (!value.IsObject())
    {
        throw rpc::Error(
            -32602,
            "Path anchor must be an object.");
    }

    const auto& anchor =
        value.AsObject();
    const std::string kind =
        RequireString(
            anchor,
            "kind");

    if (kind == "frame")
    {
        const auto frame =
            frames::FrameId::Parse(
                RequireString(
                    anchor,
                    "frame"));

        if (!frame.has_value())
        {
            throw rpc::Error(
                -32602,
                "Invalid path anchor frame ID.");
        }

        return paths::FramePointAnchor{
            .frame = *frame,
            .localMeters =
                RequireDouble3(
                    Require(
                        anchor,
                        "position"),
                    "anchor.position")
        };
    }

    if (kind == "surface")
    {
        const auto body =
            universe::BodyId::Parse(
                RequireString(
                    anchor,
                    "body"));

        if (!body.has_value())
        {
            throw rpc::Error(
                -32602,
                "Invalid path anchor body ID.");
        }

        return paths::SurfaceAnchor{
            .body = *body,
            .coordinate =
                RequireDouble3(
                    Require(
                        anchor,
                        "coordinate"),
                    "anchor.coordinate")
        };
    }

    if (kind == "entity_socket")
    {
        return paths::EntitySocketAnchor{
            .entity =
                RequireObjectId(
                    anchor,
                    "entity"),
            .socket =
                RequireString(
                    anchor,
                    "socket"),
            .localMeters =
                RequireDouble3(
                    Require(
                        anchor,
                        "position"),
                    "anchor.position")
        };
    }

    throw rpc::Error(
        -32602,
        "Path anchor kind must be frame, surface, or entity_socket.");
}

[[nodiscard]] rpc::Value
PathAnchorToRpc(
    const paths::PathAnchor& anchor)
{
    return std::visit(
        [](const auto& value)
            -> rpc::Value
        {
            using Anchor =
                std::decay_t<
                    decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Anchor,
                    paths::FramePointAnchor>)
            {
                return rpc::Value(
                    rpc::Value::Object{
                        {"kind", "frame"},
                        {
                            "frame",
                            value.frame.ToString()
                        },
                        {
                            "position",
                            rpc::Value::Array{
                                value.localMeters.x,
                                value.localMeters.y,
                                value.localMeters.z
                            }
                        }
                    });
            }
            else if constexpr (
                std::is_same_v<
                    Anchor,
                    paths::SurfaceAnchor>)
            {
                return rpc::Value(
                    rpc::Value::Object{
                        {"kind", "surface"},
                        {
                            "body",
                            value.body.ToString()
                        },
                        {
                            "coordinate",
                            rpc::Value::Array{
                                value.coordinate.x,
                                value.coordinate.y,
                                value.coordinate.z
                            }
                        }
                    });
            }
            else
            {
                return rpc::Value(
                    rpc::Value::Object{
                        {
                            "kind",
                            "entity_socket"
                        },
                        {
                            "entity",
                            value.entity.ToString()
                        },
                        {
                            "socket",
                            value.socket
                        },
                        {
                            "position",
                            rpc::Value::Array{
                                value.localMeters.x,
                                value.localMeters.y,
                                value.localMeters.z
                            }
                        }
                    });
            }
        },
        anchor);
}

[[nodiscard]] rpc::Value
PathNetworkToRpc(
    const paths::PathNetworkRecord& network)
{
    return rpc::Value(
        rpc::Value::Object{
            {"id", network.id.ToString()},
            {
                "object",
                network.object.ToString()
            },
            {"name", network.name},
            {
                "profile_asset",
                network.profileAsset
            }
        });
}

[[nodiscard]] rpc::Value
PathNodeToRpc(
    const paths::PathNodeRecord& node)
{
    return rpc::Value(
        rpc::Value::Object{
            {"id", node.id.ToString()},
            {
                "network",
                node.network.ToString()
            },
            {"name", node.name},
            {
                "anchor",
                PathAnchorToRpc(
                    node.anchor)
            }
        });
}

[[nodiscard]] rpc::Value
PathEdgeToRpc(
    const paths::PathEdgeRecord& edge)
{
    return rpc::Value(
        rpc::Value::Object{
            {"id", edge.id.ToString()},
            {
                "network",
                edge.network.ToString()
            },
            {
                "start",
                edge.startNode.ToString()
            },
            {
                "end",
                edge.endNode.ToString()
            },
            {
                "mode",
                edge.mode ==
                        paths::EdgeMode::Bezier
                    ? "bezier"
                    : (edge.mode ==
                               paths::EdgeMode::Routed
                           ? "routed"
                           : "direct")
            },
            {
                "start_handle",
                rpc::Value::Array{
                    edge.startHandleMeters.x,
                    edge.startHandleMeters.y,
                    edge.startHandleMeters.z
                }
            },
            {
                "end_handle",
                rpc::Value::Array{
                    edge.endHandleMeters.x,
                    edge.endHandleMeters.y,
                    edge.endHandleMeters.z
                }
            },
            {
                "profile_override",
                edge.profileOverride
            }
        });
}

[[nodiscard]] math::Float3
RequireFloat3(
    const rpc::Value& value,
    const std::string_view name)
{
    const math::Double3 parsed =
        RequireDouble3(
            value,
            name);

    return {
        static_cast<f32>(parsed.x),
        static_cast<f32>(parsed.y),
        static_cast<f32>(parsed.z)
    };
}
} // namespace

EditorRpcService::EditorRpcService(
    rpc::Dispatcher& dispatcher,
    const documents::ProjectDocument& project,
    commands::CommandRegistry& commandRegistry,
    commands::CommandService& commandService,
    const schema::SchemaRegistry& schemas,
    scene::ObjectStore& objects,
    selection::SelectionService& selection,
    ViewportAutomation viewport)
    : dispatcher_(dispatcher)
{
    Register(
        {
            .name = "project.info",
            .description =
                "Returns the currently open Orbit project.",
            .mutating = false
        },
        [&project](const rpc::Value&)
        {
            return rpc::Value(
                rpc::Value::Object{
                    {
                        "id",
                        project.Manifest().
                            projectId.ToString()
                    },
                    {
                        "name",
                        project.Manifest().
                            displayName
                    },
                    {
                        "root",
                        project.RootDirectory().
                            generic_string()
                    },
                    {
                        "startup_world",
                        project.Manifest().
                            startupWorld.
                            generic_string()
                    }
                });
        });

    Register(
        {
            .name = "schema.catalog",
            .description =
                "Returns registered semantic object and property schemas.",
            .mutating = false
        },
        [&schemas](const rpc::Value&)
        {
            rpc::Value::Array types;

            for (const auto& type :
                 schemas.Catalog())
            {
                rpc::Value::Array properties;

                for (const auto& property :
                     type.properties)
                {
                    rpc::Value::Object item{
                        {"id", property.id.ToString()},
                        {"name", property.name},
                        {
                            "kind",
                            PropertyKindName(
                                property.kind)
                        },
                        {"unit", property.unit},
                        {
                            "read_only",
                            property.readOnly
                        },
                        {
                            "advanced",
                            property.advanced
                        },
                        {
                            "default",
                            PropertyValueToRpc(
                                property.
                                    defaultValue)
                        }
                    };

                    if (property.range.
                            minimum.
                            has_value())
                    {
                        item.emplace(
                            "minimum",
                            *property.range.
                                minimum);
                    }

                    if (property.range.
                            maximum.
                            has_value())
                    {
                        item.emplace(
                            "maximum",
                            *property.range.
                                maximum);
                    }

                    properties.emplace_back(
                        std::move(item));
                }

                types.emplace_back(
                    rpc::Value::Object{
                        {"id", type.id.ToString()},
                        {
                            "name",
                            type.displayName
                        },
                        {
                            "category",
                            type.category
                        },
                        {
                            "properties",
                            std::move(properties)
                        }
                    });
            }

            return rpc::Value(
                std::move(types));
        });

    Register(
        {
            .name = "command.catalog",
            .description =
                "Returns the command catalog with live enablement.",
            .mutating = false
        },
        [&commandRegistry](const rpc::Value&)
        {
            rpc::Value::Array commands;

            for (const auto& command :
                 commandRegistry.Catalog())
            {
                if (!command.automationVisible)
                {
                    continue;
                }

                rpc::Value::Array parameters;

                for (const auto& parameter :
                     command.parameters)
                {
                    parameters.emplace_back(
                        rpc::Value::Object{
                            {
                                "name",
                                parameter.name
                            },
                            {
                                "kind",
                                CommandKindName(
                                    parameter.kind)
                            },
                            {
                                "required",
                                parameter.required
                            }
                        });
                }

                const auto enablement =
                    commandRegistry.
                        Enablement(
                            command.id);

                commands.emplace_back(
                    rpc::Value::Object{
                        {
                            "id",
                            command.id.ToString()
                        },
                        {
                            "name",
                            command.name
                        },
                        {
                            "category",
                            command.category
                        },
                        {
                            "description",
                            command.description
                        },
                        {
                            "enabled",
                            enablement.enabled
                        },
                        {
                            "disabled_reason",
                            enablement.reason
                        },
                        {
                            "parameters",
                            std::move(parameters)
                        }
                    });
            }

            return rpc::Value(
                std::move(commands));
        });

    Register(
        {
            .name = "command.invoke",
            .description =
                "Invokes any registered Orbit command by stable ID.",
            .mutating = true
        },
        [&commandRegistry](
            const rpc::Value& params)
        {
            const auto& object =
                RequireObject(params);

            const rpc::Value& idValue =
                Require(object, "id");

            if (!idValue.IsString())
            {
                throw rpc::Error(
                    -32602,
                    "Command ID must be a string.");
            }

            const auto id =
                commands::CommandId::Parse(
                    idValue.AsString());

            if (!id.has_value())
            {
                throw rpc::Error(
                    -32602,
                    "Invalid command ID.");
            }

            const auto* descriptor =
                commandRegistry.Find(*id);

            if (descriptor == nullptr ||
                !descriptor->automationVisible)
            {
                throw rpc::Error(
                    -32601,
                    "Command is not available to automation.");
            }

            commands::CommandArguments
                arguments;

            if (const auto found =
                    object.find("arguments");
                found != object.end())
            {
                if (!found->second.IsObject())
                {
                    throw rpc::Error(
                        -32602,
                        "Command arguments must be an object.");
                }

                const auto& supplied =
                    found->second.AsObject();

                for (const auto& parameter :
                     descriptor->parameters)
                {
                    const auto value =
                        supplied.find(
                            parameter.name);

                    if (value ==
                        supplied.end())
                    {
                        continue;
                    }

                    arguments.emplace(
                        parameter.name,
                        RpcToCommandValue(
                            parameter.kind,
                            value->second));
                }

                for (const auto&
                         [name, value] :
                     supplied)
                {
                    static_cast<void>(value);

                    bool known = false;

                    for (const auto& parameter :
                         descriptor->parameters)
                    {
                        if (parameter.name ==
                            name)
                        {
                            known = true;
                            break;
                        }
                    }

                    if (!known)
                    {
                        throw rpc::Error(
                            -32602,
                            "Unknown command argument: " +
                                name);
                    }
                }
            }

            try
            {
                commandRegistry.Invoke(
                    *id,
                    arguments);
            }
            catch (const std::exception&
                       exception)
            {
                throw rpc::Error(
                    1000,
                    exception.what());
            }

            return rpc::Value(
                rpc::Value::Object{
                    {"ok", true}
                });
        });

    Register(
        {
            .name = "object.roots",
            .description =
                "Returns root semantic objects.",
            .mutating = false
        },
        [&objects](const rpc::Value&)
        {
            rpc::Value::Array result;

            for (const auto& object :
                 objects.Roots())
            {
                result.push_back(
                    ObjectToRpc(object));
            }

            return rpc::Value(
                std::move(result));
        });

    Register(
        {
            .name = "object.children",
            .description =
                "Returns children of one semantic object.",
            .mutating = false
        },
        [&objects](const rpc::Value& params)
        {
            const auto& values =
                RequireObject(params);

            rpc::Value::Array result;

            for (const auto& object :
                 objects.Children(
                     RequireObjectId(
                         values,
                         "parent")))
            {
                result.push_back(
                    ObjectToRpc(object));
            }

            return rpc::Value(
                std::move(result));
        });

    Register(
        {
            .name = "object.get",
            .description =
                "Returns one semantic object and its authored properties.",
            .mutating = false
        },
        [&objects, &schemas](
            const rpc::Value& params)
        {
            const auto& values =
                RequireObject(params);

            const scene::ObjectId id =
                RequireObjectId(
                    values,
                    "id");

            const auto object =
                objects.Find(id);

            if (!object.has_value())
            {
                throw rpc::Error(
                    1004,
                    "Object not found.");
            }

            rpc::Value result =
                ObjectToRpc(*object);

            rpc::Value::Object properties;

            if (const auto* type =
                    schemas.FindType(
                        object->type);
                type != nullptr)
            {
                for (const auto& property :
                     type->properties)
                {
                    const auto value =
                        objects.GetProperty(
                            id,
                            property.id);

                    if (value.has_value())
                    {
                        properties.emplace(
                            property.id.
                                ToString(),
                            PropertyValueToRpc(
                                *value));
                    }
                }
            }

            result.AsObject().emplace(
                "properties",
                std::move(properties));

            return result;
        });

    Register(
        {
            .name = "object.create",
            .description =
                "Creates a semantic object through CommandService.",
            .mutating = true
        },
        [&commandService](
            const rpc::Value& params)
        {
            const auto& values =
                RequireObject(params);

            const auto& name =
                Require(
                    values,
                    "name");

            if (!name.IsString() ||
                name.AsString().empty())
            {
                throw rpc::Error(
                    -32602,
                    "Object name must be a non-empty string.");
            }

            const auto id =
                commandService.CreateObject(
                    RequireTypeId(
                        values,
                        "type"),
                    name.AsString(),
                    OptionalObjectId(
                        values,
                        "parent"));

            return rpc::Value(
                rpc::Value::Object{
                    {"id", id.ToString()}
                });
        });

    Register(
        {
            .name = "object.rename",
            .description =
                "Renames a semantic object through CommandService.",
            .mutating = true
        },
        [&commandService](
            const rpc::Value& params)
        {
            const auto& values =
                RequireObject(params);
            const auto& name =
                Require(values, "name");

            if (!name.IsString())
            {
                throw rpc::Error(
                    -32602,
                    "Object name must be a string.");
            }

            commandService.RenameObject(
                RequireObjectId(
                    values,
                    "id"),
                name.AsString());

            return rpc::Value(
                rpc::Value::Object{
                    {"ok", true}
                });
        });

    Register(
        {
            .name = "object.reparent",
            .description =
                "Reparents a semantic object through CommandService.",
            .mutating = true
        },
        [&commandService](
            const rpc::Value& params)
        {
            const auto& values =
                RequireObject(params);

            commandService.ReparentObject(
                RequireObjectId(
                    values,
                    "id"),
                OptionalObjectId(
                    values,
                    "parent"));

            return rpc::Value(
                rpc::Value::Object{
                    {"ok", true}
                });
        });

    Register(
        {
            .name = "property.set",
            .description =
                "Sets a schema property through CommandService.",
            .mutating = true
        },
        [&commandService,
         &objects,
         &schemas](
            const rpc::Value& params)
        {
            const auto& values =
                RequireObject(params);

            const scene::ObjectId objectId =
                RequireObjectId(
                    values,
                    "object");

            const auto object =
                objects.Find(
                    objectId);

            if (!object.has_value())
            {
                throw rpc::Error(
                    1004,
                    "Object not found.");
            }

            const schema::PropertyId propertyId =
                RequirePropertyId(
                    values,
                    "property");

            const auto* property =
                schemas.FindProperty(
                    object->type,
                    propertyId);

            if (property == nullptr)
            {
                throw rpc::Error(
                    1005,
                    "Property is not defined for this object type.");
            }

            commandService.SetProperty(
                objectId,
                propertyId,
                RpcToPropertyValue(
                    *property,
                    Require(
                        values,
                        "value")));

            return rpc::Value(
                rpc::Value::Object{
                    {"ok", true}
                });
        });

    Register(
        {
            .name = "path.create_network",
            .description =
                "Creates a semantic path network through the shared command layer.",
            .mutating = true
        },
        [&objects,
         &commandService](
            const rpc::Value& params)
        {
            const auto& values =
                RequireObject(params);

            paths::PathNetworkService
                service(
                    objects,
                    commandService);

            const auto result =
                service.CreateNetwork(
                    RequireString(
                        values,
                        "name"),
                    OptionalObjectId(
                        values,
                        "parent"),
                    OptionalString(
                        values,
                        "profile_asset").
                        value_or(
                            std::string{}));

            return PathNetworkToRpc(
                result);
        });

    Register(
        {
            .name = "path.create_node",
            .description =
                "Creates a frame, surface, or entity/socket anchored path node.",
            .mutating = true
        },
        [&objects,
         &commandService](
            const rpc::Value& params)
        {
            const auto& values =
                RequireObject(params);

            paths::PathNetworkService
                service(
                    objects,
                    commandService);

            const auto result =
                service.CreateNode(
                    RequireNetworkId(
                        values,
                        "network"),
                    RequireString(
                        values,
                        "name"),
                    PathAnchorFromRpc(
                        Require(
                            values,
                            "anchor")));

            return PathNodeToRpc(
                result);
        });

    Register(
        {
            .name = "path.connect",
            .description =
                "Connects two path nodes with a Direct or Bezier edge.",
            .mutating = true
        },
        [&objects,
         &commandService](
            const rpc::Value& params)
        {
            const auto& values =
                RequireObject(params);
            const scene::ObjectId start =
                RequireObjectId(
                    values,
                    "start");
            const scene::ObjectId end =
                RequireObjectId(
                    values,
                    "end");
            const std::string mode =
                RequireString(
                    values,
                    "mode");
            const auto name =
                OptionalString(
                    values,
                    "name");

            paths::PathNetworkService
                service(
                    objects,
                    commandService);

            if (mode == "direct")
            {
                return PathEdgeToRpc(
                    service.ConnectDirect(
                        start,
                        end,
                        name.value_or(
                            "Direct Edge")));
            }

            if (mode == "routed")
            {
                return PathEdgeToRpc(
                    service.ConnectRouted(
                        start,
                        end,
                        name.value_or(
                            "Routed Edge")));
            }

            if (mode == "bezier")
            {
                math::Double3 startHandle{};
                math::Double3 endHandle{};

                if (const auto found =
                        values.find(
                            "start_handle");
                    found != values.end())
                {
                    startHandle =
                        RequireDouble3(
                            found->second,
                            "start_handle");
                }

                if (const auto found =
                        values.find(
                            "end_handle");
                    found != values.end())
                {
                    endHandle =
                        RequireDouble3(
                            found->second,
                            "end_handle");
                }

                return PathEdgeToRpc(
                    service.ConnectBezier(
                        start,
                        end,
                        startHandle,
                        endHandle,
                        name.value_or(
                            "Bezier Edge")));
            }

            throw rpc::Error(
                -32602,
                "Path connection mode must be direct, bezier, or routed.");
        });

    Register(
        {
            .name = "path.set_bezier_handles",
            .description =
                "Edits the two local Bezier handles transactionally.",
            .mutating = true
        },
        [&objects,
         &commandService](
            const rpc::Value& params)
        {
            const auto& values =
                RequireObject(params);

            paths::PathNetworkService
                service(
                    objects,
                    commandService);

            const scene::ObjectId edge =
                RequireObjectId(
                    values,
                    "edge");

            service.SetBezierHandles(
                edge,
                RequireDouble3(
                    Require(
                        values,
                        "start_handle"),
                    "start_handle"),
                RequireDouble3(
                    Require(
                        values,
                        "end_handle"),
                    "end_handle"));

            const auto result =
                service.FindEdge(edge);

            if (!result.has_value())
            {
                throw rpc::Error(
                    1010,
                    "Path edge disappeared after edit.");
            }

            return PathEdgeToRpc(
                *result);
        });

    Register(
        {
            .name = "path.set_profile",
            .description =
                "Assigns or clears a path-profile asset on a network or edge override.",
            .mutating = true
        },
        [&objects,
         &commandService](
            const rpc::Value& params)
        {
            const auto& values =
                RequireObject(params);

            paths::PathNetworkService
                service(
                    objects,
                    commandService);

            const scene::ObjectId object =
                RequireObjectId(
                    values,
                    "object");

            std::string profile;

            if (const auto found =
                    values.find(
                        "profile_asset");
                found != values.end() &&
                !found->second.IsNull())
            {
                if (!found->second.IsString())
                {
                    throw rpc::Error(
                        -32602,
                        "profile_asset must be a string or null.");
                }

                profile =
                    found->second.AsString();
            }

            service.SetProfile(
                object,
                profile);

            return rpc::Value(
                rpc::Value::Object{
                    {"ok", true},
                    {
                        "object",
                        object.ToString()
                    },
                    {
                        "profile_asset",
                        profile
                    }
                });
        });

    Register(
        {
            .name = "path.inspect",
            .description =
                "Returns semantic path network, node, or edge data for an object ID.",
            .mutating = false
        },
        [&objects,
         &commandService](
            const rpc::Value& params)
        {
            const auto& values =
                RequireObject(params);
            const scene::ObjectId id =
                RequireObjectId(
                    values,
                    "id");
            const auto object =
                objects.Find(id);

            if (!object.has_value())
            {
                throw rpc::Error(
                    1001,
                    "Path object does not exist.");
            }

            paths::PathNetworkService
                service(
                    objects,
                    commandService);

            if (object->type ==
                paths::kPathNetworkType)
            {
                const paths::NetworkId
                    network{
                        .high = id.high,
                        .low = id.low
                    };

                const auto result =
                    service.FindNetwork(
                        network);

                if (!result.has_value())
                {
                    throw rpc::Error(
                        1011,
                        "Path network is invalid.");
                }

                return PathNetworkToRpc(
                    *result);
            }

            if (object->type ==
                paths::kPathNodeType)
            {
                const auto result =
                    service.FindNode(id);

                if (!result.has_value())
                {
                    throw rpc::Error(
                        1012,
                        "Path node is invalid.");
                }

                return PathNodeToRpc(
                    *result);
            }

            if (object->type ==
                paths::kPathEdgeType)
            {
                const auto result =
                    service.FindEdge(id);

                if (!result.has_value())
                {
                    throw rpc::Error(
                        1013,
                        "Path edge is invalid.");
                }

                return PathEdgeToRpc(
                    *result);
            }

            throw rpc::Error(
                -32602,
                "Object is not a path network, node, or edge.");
        });

    Register(
        {
            .name = "selection.get",
            .description =
                "Returns the ordered shared editor selection.",
            .mutating = false
        },
        [&selection](const rpc::Value&)
        {
            rpc::Value::Array result;

            for (const auto id :
                 selection.Ordered())
            {
                result.emplace_back(
                    id.ToString());
            }

            return rpc::Value(
                std::move(result));
        });

    Register(
        {
            .name = "selection.set",
            .description =
                "Replaces the shared editor selection.",
            .mutating = true
        },
        [&selection, &objects](
            const rpc::Value& params)
        {
            const auto& values =
                RequireObject(params);

            const rpc::Value& ids =
                Require(values, "ids");

            if (!ids.IsArray())
            {
                throw rpc::Error(
                    -32602,
                    "ids must be an array.");
            }

            std::vector<scene::ObjectId>
                selected;

            for (const rpc::Value& value :
                 ids.AsArray())
            {
                if (!value.IsString())
                {
                    throw rpc::Error(
                        -32602,
                        "Selection IDs must be strings.");
                }

                const auto id =
                    scene::ObjectId::Parse(
                        value.AsString());

                if (!id.has_value() ||
                    !objects.Find(*id).
                        has_value())
                {
                    throw rpc::Error(
                        -32602,
                        "Selection contains an invalid or missing object.");
                }

                selected.push_back(*id);
            }

            selection.Set(
                std::span(selected));

            return rpc::Value(
                rpc::Value::Object{
                    {"revision",
                     static_cast<i64>(
                         selection.Revision())}
                });
        });

    Register(
        {
            .name = "selection.clear",
            .description =
                "Clears the shared editor selection.",
            .mutating = true
        },
        [&selection](const rpc::Value&)
        {
            selection.Clear();

            return rpc::Value(
                rpc::Value::Object{
                    {"revision",
                     static_cast<i64>(
                         selection.Revision())}
                });
        });

    Register(
        {
            .name = "transaction.begin",
            .description =
                "Begins an atomic authoring transaction.",
            .mutating = true
        },
        [&commandService](
            const rpc::Value& params)
        {
            const auto& values =
                RequireObject(params);
            const auto& label =
                Require(values, "label");

            if (!label.IsString() ||
                label.AsString().empty())
            {
                throw rpc::Error(
                    -32602,
                    "Transaction label must be a non-empty string.");
            }

            commandService.BeginTransaction(
                label.AsString());

            return rpc::Value(
                rpc::Value::Object{
                    {"ok", true}
                });
        });

    Register(
        {
            .name = "transaction.commit",
            .description =
                "Commits the active atomic authoring transaction.",
            .mutating = true
        },
        [&commandService](
            const rpc::Value&)
        {
            commandService.
                CommitTransaction();

            return rpc::Value(
                rpc::Value::Object{
                    {"ok", true}
                });
        });

    Register(
        {
            .name = "transaction.rollback",
            .description =
                "Rolls back the active atomic authoring transaction.",
            .mutating = true
        },
        [&commandService](
            const rpc::Value&)
        {
            commandService.
                RollbackTransaction();

            return rpc::Value(
                rpc::Value::Object{
                    {"ok", true}
                });
        });

    Register(
        {
            .name = "history.undo",
            .description =
                "Undoes the last committed authoring transaction.",
            .mutating = true
        },
        [&commandService](
            const rpc::Value&)
        {
            if (!commandService.CanUndo())
            {
                throw rpc::Error(
                    1006,
                    "Nothing to undo.");
            }

            commandService.Undo();

            return rpc::Value(
                rpc::Value::Object{
                    {"ok", true}
                });
        });

    Register(
        {
            .name = "history.redo",
            .description =
                "Redoes the most recently undone authoring transaction.",
            .mutating = true
        },
        [&commandService](
            const rpc::Value&)
        {
            if (!commandService.CanRedo())
            {
                throw rpc::Error(
                    1007,
                    "Nothing to redo.");
            }

            commandService.Redo();

            return rpc::Value(
                rpc::Value::Object{
                    {"ok", true}
                });
        });

    Register(
        {
            .name = "event.since",
            .description =
                "Returns replayable editor events newer than a sequence number.",
            .mutating = false
        },
        [this](const rpc::Value& params)
        {
            i64 after = 0;

            if (params.IsObject())
            {
                if (const auto* value =
                        params.Find("sequence");
                    value != nullptr)
                {
                    try
                    {
                        after =
                            value->AsInteger();
                    }
                    catch (const std::bad_variant_access&)
                    {
                        throw rpc::Error(
                            -32602,
                            "event.since sequence must be an integer.");
                    }
                }
            }
            else
            {
                throw rpc::Error(
                    -32602,
                    "event.since params must be an object.");
            }

            if (after < 0)
            {
                throw rpc::Error(
                    -32602,
                    "event.since sequence must not be negative.");
            }

            rpc::Value::Array events;

            for (const EventRecord& event :
                 events_)
            {
                if (event.sequence <=
                    static_cast<u64>(after))
                {
                    continue;
                }

                events.emplace_back(
                    rpc::Value::Object{
                        {
                            "sequence",
                            static_cast<i64>(
                                event.sequence)
                        },
                        {"type", event.type},
                        {"data", event.data}
                    });
            }

            const u64 oldestSequence =
                events_.empty()
                    ? nextEventSequence_
                    : events_.front().
                        sequence;

            const bool truncated =
                !events_.empty() &&
                static_cast<u64>(after) + 1U <
                    oldestSequence;

            return rpc::Value(
                rpc::Value::Object{
                    {
                        "oldest_sequence",
                        static_cast<i64>(
                            oldestSequence)
                    },
                    {
                        "latest_sequence",
                        static_cast<i64>(
                            LatestEventSequence())
                    },
                    {
                        "truncated",
                        truncated
                    },
                    {
                        "events",
                        std::move(events)
                    }
                });
        });

    AttachViewport(
        std::move(viewport));
}

void EditorRpcService::AttachViewport(
    ViewportAutomation viewport)
{
    if (viewport.view == nullptr)
    {
        return;
    }

    if (viewportRegistered_)
    {
        throw std::logic_error(
            "Editor RPC viewport is already attached.");
    }

    viewportRegistered_ = true;

    Register(
            {
                .name = "viewport.get",
                .description =
                    "Returns the primary Studio RenderView state and dimensions.",
                .mutating = false
            },
            [view = viewport.view](
                const rpc::Value&)
            {
                return ViewportToRpc(
                    *view);
            });

        Register(
            {
                .name = "viewport.set_camera",
                .description =
                    "Updates primary RenderView camera state.",
                .mutating = true
            },
            [this,
             view = viewport.view](
                const rpc::Value& params)
            {
                const auto& values =
                    RequireObject(params);

                auto camera =
                    view->Camera();

                if (const auto found =
                        values.find("frame");
                    found != values.end())
                {
                    if (found->second.IsNull())
                    {
                        camera.frame = {};
                    }
                    else if (found->second.IsString())
                    {
                        const auto frame =
                            frames::FrameId::Parse(
                                found->second.
                                    AsString());

                        if (!frame.has_value())
                        {
                            throw rpc::Error(
                                -32602,
                                "Invalid viewport frame ID.");
                        }

                        camera.frame = *frame;
                    }
                    else
                    {
                        throw rpc::Error(
                            -32602,
                            "viewport frame must be a string or null.");
                    }
                }

                if (const auto found =
                        values.find("position");
                    found != values.end())
                {
                    camera.localPositionMeters =
                        RequireDouble3(
                            found->second,
                            "position");
                }

                if (const auto found =
                        values.find("forward");
                    found != values.end())
                {
                    camera.forward =
                        RequireFloat3(
                            found->second,
                            "forward");
                }

                if (const auto found =
                        values.find("up");
                    found != values.end())
                {
                    camera.up =
                        RequireFloat3(
                            found->second,
                            "up");
                }

                if (const auto found =
                        values.find(
                            "vertical_fov_radians");
                    found != values.end())
                {
                    const f64 fov =
                        RequireNumber(
                            found->second,
                            "vertical_fov_radians");

                    if (fov <= 0.0 ||
                        fov >= 3.13)
                    {
                        throw rpc::Error(
                            -32602,
                            "vertical_fov_radians must be between 0 and pi.");
                    }

                    camera.verticalFovRadians =
                        static_cast<f32>(
                            fov);
                }

                if (const auto found =
                        values.find(
                            "near_plane_meters");
                    found != values.end())
                {
                    const f64 nearPlane =
                        RequireNumber(
                            found->second,
                            "near_plane_meters");

                    if (nearPlane <= 0.0)
                    {
                        throw rpc::Error(
                            -32602,
                            "near_plane_meters must be positive.");
                    }

                    camera.nearPlaneMeters =
                        static_cast<f32>(
                            nearPlane);
                }

                if (const auto found =
                        values.find(
                            "far_plane_meters");
                    found != values.end())
                {
                    const f64 farPlane =
                        RequireNumber(
                            found->second,
                            "far_plane_meters");

                    if (farPlane <=
                        camera.nearPlaneMeters)
                    {
                        throw rpc::Error(
                            -32602,
                            "far_plane_meters must exceed near_plane_meters.");
                    }

                    camera.farPlaneMeters =
                        static_cast<f32>(
                            farPlane);
                }

                if (camera.farPlaneMeters <=
                    camera.nearPlaneMeters)
                {
                    throw rpc::Error(
                        -32602,
                        "Viewport far plane must exceed its near plane.");
                }

                if (math::Length(
                        camera.forward) <=
                        0.0001F ||
                    math::Length(
                        camera.up) <=
                        0.0001F)
                {
                    throw rpc::Error(
                        -32602,
                        "Viewport forward and up vectors must be non-zero.");
                }

                camera.forward =
                    math::Normalize(
                        camera.forward);
                camera.up =
                    math::Normalize(
                        camera.up);

                if (std::abs(
                        math::Dot(
                            camera.forward,
                            camera.up)) >
                    0.999F)
                {
                    throw rpc::Error(
                        -32602,
                        "Viewport forward and up vectors must not be parallel.");
                }

                view->Camera() =
                    camera;

                PublishEvent(
                    "viewport.changed",
                    ViewportToRpc(*view));

                return ViewportToRpc(
                    *view);
            });

        if (viewport.capture)
        {
            Register(
                {
                    .name = "viewport.screenshot",
                    .description =
                        "Captures the completed primary RenderView to a BMP file.",
                    .mutating = false
                },
                [this,
                 capture =
                     std::move(
                         viewport.capture)](
                    const rpc::Value& params)
                {
                    const auto& values =
                        RequireObject(params);

                    const auto& pathValue =
                        Require(
                            values,
                            "path");

                    if (!pathValue.IsString() ||
                        pathValue.AsString().
                            empty())
                    {
                        throw rpc::Error(
                            -32602,
                            "viewport.screenshot path must be a non-empty string.");
                    }

                    const auto captured =
                        capture(
                            pathValue.
                                AsString());

                    rpc::Value data(
                        rpc::Value::Object{
                            {
                                "path",
                                captured.path.
                                    generic_string()
                            },
                            {
                                "width",
                                static_cast<i64>(
                                    captured.width)
                            },
                            {
                                "height",
                                static_cast<i64>(
                                    captured.height)
                            },
                            {
                                "file_bytes",
                                static_cast<i64>(
                                    captured.fileBytes)
                            }
                        });

                    PublishEvent(
                        "viewport.captured",
                        data);

                    return data;
                });
        }
}

void EditorRpcService::AttachPathRouting(
    PathRoutingAutomation routing)
{
    if (!routing.status &&
        !routing.result &&
        !routing.invalidate)
    {
        return;
    }

    if (pathRoutingRegistered_)
    {
        throw std::logic_error(
            "Editor RPC path routing is already attached.");
    }

    pathRoutingRegistered_ = true;

    if (routing.status)
    {
        Register(
            {
                .name = "path.route_status",
                .description =
                    "Returns derived routing state for a routed PathEdge.",
                .mutating = false
            },
            [status =
                 std::move(
                     routing.status)](
                const rpc::Value& params)
            {
                const auto& values =
                    RequireObject(params);

                return status(
                    RequireObjectId(
                        values,
                        "edge"));
            });
    }

    if (routing.result)
    {
        Register(
            {
                .name = "path.route_result",
                .description =
                    "Returns the current derived routed polyline and cost.",
                .mutating = false
            },
            [result =
                 std::move(
                     routing.result)](
                const rpc::Value& params)
            {
                const auto& values =
                    RequireObject(params);

                return result(
                    RequireObjectId(
                        values,
                        "edge"));
            });
    }

    if (routing.invalidate)
    {
        Register(
            {
                .name = "path.route_invalidate",
                .description =
                    "Invalidates one derived route without mutating project authority.",
                .mutating = true
            },
            [invalidate =
                 std::move(
                     routing.invalidate)](
                const rpc::Value& params)
            {
                const auto& values =
                    RequireObject(params);
                const scene::ObjectId edge =
                    RequireObjectId(
                        values,
                        "edge");

                invalidate(edge);

                return rpc::Value(
                    rpc::Value::Object{
                        {"ok", true},
                        {
                            "edge",
                            edge.ToString()
                        }
                    });
            });
    }
}

void EditorRpcService::AttachPathGeometry(
    PathGeometryAutomation geometry)
{
    if (!geometry.result)
    {
        return;
    }

    if (pathGeometryRegistered_)
    {
        throw std::logic_error(
            "Editor RPC path geometry is already attached.");
    }

    pathGeometryRegistered_ = true;

    Register(
        {
            .name = "path.derived_result",
            .description =
                "Returns deterministic M20 visual/AI/collision derived state for a PathEdge.",
            .mutating = false
        },
        [result =
             std::move(
                 geometry.result)](
            const rpc::Value& params)
        {
            const auto& values =
                RequireObject(params);

            return result(
                RequireObjectId(
                    values,
                    "edge"));
        });
}

void EditorRpcService::AttachBuild(
    BuildAutomation build)
{
    if (!build.profiles &&
        !build.validate &&
        !build.cook &&
        !build.package)
    {
        return;
    }

    if (buildRegistered_)
    {
        throw std::logic_error(
            "Editor RPC build automation is already attached.");
    }

    buildRegistered_ = true;

    if (build.profiles)
    {
        Register(
            {
                .name = "build.profiles",
                .description =
                    "Returns project build profiles available to Studio and OrbitBuild.",
                .mutating = false
            },
            [profiles =
                 std::move(
                     build.profiles)](
                const rpc::Value&)
            {
                return profiles();
            });
    }

    if (build.validate)
    {
        Register(
            {
                .name = "build.validate",
                .description =
                    "Validates one project build profile through the shared BuildService.",
                .mutating = false
            },
            [validate =
                 std::move(
                     build.validate)](
                const rpc::Value& params)
            {
                std::optional<std::string>
                    profile;

                if (params.IsObject())
                {
                    profile =
                        OptionalString(
                            params.AsObject(),
                            "profile");
                }
                else if (!params.IsNull())
                {
                    throw rpc::Error(
                        -32602,
                        "build.validate params must be an object or null.");
                }

                return validate(
                    std::move(profile));
            });
    }

    if (build.cook)
    {
        Register(
            {
                .name = "build.cook",
                .description =
                    "Checkpoints the open project and cooks one build profile through the shared BuildService.",
                .mutating = true
            },
            [this,
             cook =
                 std::move(
                     build.cook)](
                const rpc::Value& params)
            {
                std::optional<std::string>
                    profile;

                if (params.IsObject())
                {
                    profile =
                        OptionalString(
                            params.AsObject(),
                            "profile");
                }
                else if (!params.IsNull())
                {
                    throw rpc::Error(
                        -32602,
                        "build.cook params must be an object or null.");
                }

                PublishEvent(
                    "build.started",
                    rpc::Value(
                        rpc::Value::Object{
                            {
                                "profile",
                                profile.
                                    value_or(
                                        std::string{})
                            }
                        }));

                try
                {
                    rpc::Value result =
                        cook(
                            std::move(profile));

                    PublishEvent(
                        "build.completed",
                        result);

                    return result;
                }
                catch (const std::exception&
                           exception)
                {
                    PublishEvent(
                        "build.failed",
                        rpc::Value(
                            rpc::Value::Object{
                                {
                                    "message",
                                    exception.what()
                                }
                            }));
                    throw;
                }
            });
    }

    if (build.package)
    {
        Register(
            {
                .name = "build.package",
                .description =
                    "Checkpoints the open project and assembles a standalone OrbitPlayer package.",
                .mutating = true
            },
            [this,
             package =
                 std::move(
                     build.package)](
                const rpc::Value& params)
            {
                std::optional<std::string>
                    profile;

                if (params.IsObject())
                {
                    profile =
                        OptionalString(
                            params.AsObject(),
                            "profile");
                }
                else if (!params.IsNull())
                {
                    throw rpc::Error(
                        -32602,
                        "build.package params must be an object or null.");
                }

                PublishEvent(
                    "build.package_started",
                    rpc::Value(
                        rpc::Value::Object{
                            {
                                "profile",
                                profile.
                                    value_or(
                                        std::string{})
                            }
                        }));

                try
                {
                    rpc::Value result =
                        package(
                            std::move(profile));

                    PublishEvent(
                        "build.package_completed",
                        result);

                    return result;
                }
                catch (const std::exception&
                           exception)
                {
                    PublishEvent(
                        "build.package_failed",
                        rpc::Value(
                            rpc::Value::Object{
                                {
                                    "message",
                                    exception.what()
                                }
                            }));
                    throw;
                }
            });
    }
}

void EditorRpcService::PublishEvent(
    std::string type,
    rpc::Value data)
{
    if (type.empty())
    {
        throw std::invalid_argument(
            "Editor RPC event type must not be empty.");
    }

    const u64 sequence =
        nextEventSequence_++;

    events_.push_back({
        .sequence = sequence,
        .type = type,
        .data = data
    });

    constexpr std::size_t
        kMaximumRetainedEvents = 512;

    while (events_.size() >
           kMaximumRetainedEvents)
    {
        events_.pop_front();
    }

    pendingNotifications_.push_back(
        rpc::Serialize(
            rpc::Value(
                rpc::Value::Object{
                    {"jsonrpc", "2.0"},
                    {
                        "method",
                        "event." + type
                    },
                    {
                        "params",
                        rpc::Value::Object{
                            {
                                "sequence",
                                static_cast<i64>(
                                    sequence)
                            },
                            {"type", type},
                            {"data", std::move(data)}
                        }
                    }
                })));
}

std::vector<std::string>
EditorRpcService::DrainNotifications()
{
    std::vector<std::string> result;
    result.swap(
        pendingNotifications_);
    return result;
}

u64 EditorRpcService::LatestEventSequence()
    const noexcept
{
    return nextEventSequence_ - 1U;
}

EditorRpcService::~EditorRpcService()
{
    for (const std::string& method :
         registeredMethods_)
    {
        static_cast<void>(
            dispatcher_.Unregister(
                method));
    }
}

void EditorRpcService::Register(
    rpc::MethodDescriptor descriptor,
    rpc::Dispatcher::MethodHandler handler)
{
    registeredMethods_.push_back(
        descriptor.name);

    try
    {
        dispatcher_.Register(
            std::move(descriptor),
            std::move(handler));
    }
    catch (...)
    {
        registeredMethods_.pop_back();
        throw;
    }
}
} // namespace orbit::editor_rpc
