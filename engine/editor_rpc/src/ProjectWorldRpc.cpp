#include <orbit/editor_rpc/EditorRpcService.hpp>

#include <stdexcept>
#include <string>
#include <utility>

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
            "World method params must be an object.");
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
    RegisterProjectWorldAutomation(project);
}

void EditorRpcService::RegisterProjectWorldAutomation(
    documents::ProjectDocument& project)
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
}
} // namespace orbit::editor_rpc
