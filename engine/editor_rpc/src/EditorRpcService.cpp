#include <orbit/editor_rpc/EditorRpcService.hpp>

#include <orbit/math/Vector.hpp>

#include <array>
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
} // namespace

EditorRpcService::EditorRpcService(
    rpc::Dispatcher& dispatcher,
    const documents::ProjectDocument& project,
    commands::CommandRegistry& commandRegistry,
    commands::CommandService& commandService,
    const schema::SchemaRegistry& schemas,
    scene::ObjectStore& objects,
    selection::SelectionService& selection)
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
