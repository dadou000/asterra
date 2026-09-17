#include <orbit/editor_rpc/EditorRpcService.hpp>

#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>

#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace orbit::editor_rpc
{
namespace
{
[[nodiscard]] const rpc::Value::Object&
RequireWorldParams(const rpc::Value& params)
{
    if (!params.IsObject())
    {
        throw rpc::Error(
            -32602,
            "World/body method params must be an object.");
    }

    return params.AsObject();
}

[[nodiscard]] std::string RequireWorldString(
    const rpc::Value::Object& object,
    const std::string_view key)
{
    const auto found = object.find(key);

    if (found == object.end() ||
        !found->second.IsString() ||
        found->second.AsString().empty())
    {
        throw rpc::Error(
            -32602,
            std::string(key) +
                " must be a non-empty string.");
    }

    return found->second.AsString();
}

[[nodiscard]] std::optional<std::string>
OptionalWorldString(
    const rpc::Value::Object& object,
    const std::string_view key)
{
    const auto found = object.find(key);

    if (found == object.end() ||
        found->second.IsNull())
    {
        return std::nullopt;
    }

    if (!found->second.IsString() ||
        found->second.AsString().empty())
    {
        throw rpc::Error(
            -32602,
            std::string(key) +
                " must be a non-empty string or null.");
    }

    return found->second.AsString();
}

[[nodiscard]] scene::ObjectId RequireWorldObjectId(
    const rpc::Value::Object& object,
    const std::string_view key)
{
    const auto parsed =
        scene::ObjectId::Parse(
            RequireWorldString(
                object,
                key));

    if (!parsed.has_value())
    {
        throw rpc::Error(
            -32602,
            std::string(key) +
                " is not a valid object ID.");
    }

    return *parsed;
}

[[nodiscard]] rpc::Value WorldToRpc(
    const documents::WorldDescriptor& world)
{
    return rpc::Value(
        rpc::Value::Object{
            {"id", world.id.ToString()},
            {
                "path",
                world.relativePath.generic_string()
            },
            {"display_name", world.displayName},
            {
                "schema_version",
                static_cast<i64>(
                    world.schemaVersion)
            },
            {"startup", world.startup}
        });
}

[[nodiscard]] rpc::Value BodyToRpc(
    const scene::ObjectRecord& body)
{
    rpc::Value::Object result{
        {"id", body.id.ToString()},
        {"name", body.name},
        {"type", body.type.ToString()}
    };

    result.emplace(
        "parent",
        body.parent.has_value()
            ? rpc::Value(
                  body.parent->ToString())
            : rpc::Value{});

    return rpc::Value(
        std::move(result));
}

[[nodiscard]] std::vector<scene::ObjectRecord>
CelestialBodies(scene::ObjectStore& objects)
{
    std::vector<scene::ObjectRecord> pending =
        objects.Roots();
    std::vector<scene::ObjectRecord> result;

    while (!pending.empty())
    {
        auto object =
            std::move(pending.back());
        pending.pop_back();

        if (object.type ==
            editor_model::builtin::
                kCelestialBodyType)
        {
            result.push_back(object);
        }

        auto children =
            objects.Children(object.id);
        pending.insert(
            pending.end(),
            children.begin(),
            children.end());
    }

    return result;
}

template <typename Callback>
[[nodiscard]] rpc::Value ProjectMutation(
    Callback&& callback)
{
    try
    {
        return callback();
    }
    catch (const rpc::Error&)
    {
        throw;
    }
    catch (const std::exception& exception)
    {
        throw rpc::Error(
            1020,
            exception.what());
    }
}
} // namespace

EditorRpcService::EditorRpcService(
    rpc::Dispatcher& dispatcher,
    documents::ProjectDocument& project,
    commands::CommandRegistry& commandRegistry,
    commands::CommandService& commandService,
    const schema::SchemaRegistry& schemas,
    scene::ObjectStore& objects,
    selection::SelectionService& selection,
    ViewportAutomation viewport)
    : EditorRpcService(
          dispatcher,
          static_cast<const documents::ProjectDocument&>(
              project),
          commandRegistry,
          commandService,
          schemas,
          objects,
          selection,
          std::move(viewport))
{
    RegisterProjectWorldAutomation(
        project,
        commandRegistry,
        commandService,
        objects,
        selection);
}

void EditorRpcService::RegisterProjectWorldAutomation(
    documents::ProjectDocument& project,
    commands::CommandRegistry& commandRegistry,
    commands::CommandService& commandService,
    scene::ObjectStore& objects,
    selection::SelectionService& selection)
{
    Register(
        {
            .name = "world.list",
            .description =
                "Returns the authoritative project world-document catalog.",
            .mutating = false
        },
        [&project](const rpc::Value&)
        {
            rpc::Value::Array result;

            try
            {
                for (const auto& world :
                     project.Worlds())
                {
                    result.push_back(
                        WorldToRpc(world));
                }
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(
                    1021,
                    exception.what());
            }

            return rpc::Value(
                std::move(result));
        });

    Register(
        {
            .name = "world.describe",
            .description =
                "Returns one authoritative project world descriptor.",
            .mutating = false
        },
        [&project](const rpc::Value& params)
        {
            const auto& values =
                RequireWorldParams(params);
            const std::string path =
                RequireWorldString(
                    values,
                    "path");

            try
            {
                return WorldToRpc(
                    project.DescribeWorld(path));
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(
                    1021,
                    exception.what());
            }
        });

    Register(
        {
            .name = "world.create",
            .description =
                "Creates a versioned project world through ProjectDocument.",
            .mutating = true
        },
        [this, &project](const rpc::Value& params)
        {
            const auto& values =
                RequireWorldParams(params);
            const std::string path =
                RequireWorldString(
                    values,
                    "path");
            const std::string displayName =
                RequireWorldString(
                    values,
                    "display_name");

            rpc::Value result =
                ProjectMutation(
                    [&project,
                     &path,
                     &displayName]
                    {
                        const auto created =
                            project.CreateWorld(
                                path,
                                displayName);
                        return WorldToRpc(
                            project.DescribeWorld(
                                created));
                    });

            PublishEvent(
                "world.created",
                result);
            return result;
        });

    Register(
        {
            .name = "world.set_display_name",
            .description =
                "Changes persistent display metadata for a project world without renaming its file.",
            .mutating = true
        },
        [this, &project](const rpc::Value& params)
        {
            const auto& values =
                RequireWorldParams(params);
            const std::string path =
                RequireWorldString(
                    values,
                    "path");
            const std::string displayName =
                RequireWorldString(
                    values,
                    "display_name");

            rpc::Value result =
                ProjectMutation(
                    [&project,
                     &path,
                     &displayName]
                    {
                        project.SetWorldDisplayName(
                            path,
                            displayName);
                        return WorldToRpc(
                            project.DescribeWorld(
                                path));
                    });

            PublishEvent(
                "world.metadata_changed",
                result);
            return result;
        });

    Register(
        {
            .name = "world.set_startup",
            .description =
                "Changes the persisted startup world. This does not hot-switch the active authoring session.",
            .mutating = true
        },
        [this, &project](const rpc::Value& params)
        {
            const auto& values =
                RequireWorldParams(params);
            const std::string path =
                RequireWorldString(
                    values,
                    "path");

            rpc::Value result =
                ProjectMutation(
                    [&project, &path]
                    {
                        project.SetStartupWorld(path);
                        return WorldToRpc(
                            project.DescribeWorld(
                                path));
                    });

            PublishEvent(
                "project.startup_world_changed",
                result);
            return result;
        });

    Register(
        {
            .name = "body.list",
            .description =
                "Returns celestial body semantic objects in the active authoring world.",
            .mutating = false
        },
        [&objects](const rpc::Value&)
        {
            rpc::Value::Array result;

            for (const auto& body :
                 CelestialBodies(objects))
            {
                result.push_back(
                    BodyToRpc(body));
            }

            return rpc::Value(
                std::move(result));
        });

    Register(
        {
            .name = "body.create",
            .description =
                "Creates a celestial body through the shared contextual authoring command.",
            .mutating = true
        },
        [this,
         &commandRegistry,
         &commandService,
         &objects,
         &selection](const rpc::Value& params)
        {
            const auto& values =
                RequireWorldParams(params);
            const scene::ObjectId parent =
                RequireWorldObjectId(
                    values,
                    "parent");
            const auto requestedName =
                OptionalWorldString(
                    values,
                    "name");

            if (!objects.Find(parent).has_value())
            {
                throw rpc::Error(
                    1004,
                    "Body parent object does not exist.");
            }

            const auto previousSelection =
                selection.Ordered();
            const scene::ObjectId selected[] = {
                parent
            };
            selection.Set(selected);

            const auto enablement =
                commandRegistry.Enablement(
                    editor_model::
                        authoring_commands::
                            kCreateCelestialBody);

            if (!enablement.enabled)
            {
                selection.Set(
                    std::span(previousSelection));
                throw rpc::Error(
                    1022,
                    enablement.reason.empty()
                        ? "Celestial body creation is disabled for the requested parent."
                        : enablement.reason);
            }

            const bool ownsTransaction =
                !commandService.HasActiveTransaction();

            if (ownsTransaction)
            {
                commandService.BeginTransaction(
                    "RPC Create Celestial Body");
            }

            try
            {
                commandRegistry.Invoke(
                    editor_model::
                        authoring_commands::
                            kCreateCelestialBody);

                if (selection.Ordered().size() != 1)
                {
                    throw std::runtime_error(
                        "Celestial body command did not select exactly one created body.");
                }

                const scene::ObjectId bodyId =
                    selection.Ordered().front();
                auto body = objects.Find(bodyId);

                if (!body.has_value() ||
                    body->type !=
                        editor_model::builtin::
                            kCelestialBodyType)
                {
                    throw std::runtime_error(
                        "Celestial body command did not create a body object.");
                }

                if (requestedName.has_value())
                {
                    commandService.RenameObject(
                        bodyId,
                        *requestedName);
                    body = objects.Find(bodyId);
                }

                if (ownsTransaction)
                {
                    commandService.CommitTransaction();
                }

                rpc::Value result =
                    BodyToRpc(*body);
                PublishEvent(
                    "body.created",
                    result);
                return result;
            }
            catch (const rpc::Error&)
            {
                if (ownsTransaction &&
                    commandService.HasActiveTransaction())
                {
                    commandService.RollbackTransaction();
                }
                selection.Set(
                    std::span(previousSelection));
                throw;
            }
            catch (const std::exception& exception)
            {
                if (ownsTransaction &&
                    commandService.HasActiveTransaction())
                {
                    commandService.RollbackTransaction();
                }
                selection.Set(
                    std::span(previousSelection));
                throw rpc::Error(
                    1022,
                    exception.what());
            }
        });
}
} // namespace orbit::editor_rpc
