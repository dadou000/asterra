#include <orbit/editor_rpc/EditorSessionRpcHost.hpp>

#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace orbit::editor_rpc
{
namespace
{
[[nodiscard]] const rpc::Value::Object& RequireParams(
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

[[nodiscard]] std::string RequireString(
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
    const documents::WorldDescriptor& world,
    const std::optional<u64> generation = std::nullopt)
{
    rpc::Value::Object result{
        {"id", world.id.ToString()},
        {
            "path",
            world.relativePath.generic_string()
        },
        {"display_name", world.displayName},
        {
            "schema_version",
            static_cast<i64>(world.schemaVersion)
        },
        {"startup", world.startup}
    };

    if (generation.has_value())
    {
        result.emplace(
            "generation",
            static_cast<i64>(*generation));
    }

    return rpc::Value(std::move(result));
}

[[nodiscard]] rpc::Value ProjectInfoToRpc(
    const documents::ProjectDocument& project)
{
    return rpc::Value(
        rpc::Value::Object{
            {
                "id",
                project.Manifest().projectId.ToString()
            },
            {
                "name",
                project.Manifest().displayName
            },
            {
                "root",
                project.RootDirectory().generic_string()
            },
            {
                "startup_world",
                project.Manifest().startupWorld.generic_string()
            }
        });
}

[[nodiscard]] std::string BatchSwitchError()
{
    return rpc::Serialize(
        rpc::Value(
            rpc::Value::Object{
                {"jsonrpc", "2.0"},
                {"id", rpc::Value{}},
                {
                    "error",
                    rpc::Value::Object{
                        {"code", i64{1025}},
                        {
                            "message",
                            "world.open and world.close must be dispatched as standalone requests."
                        }
                    }
                }
            }));
}
} // namespace

EditorSessionRpcHost::EditorSessionRpcHost(
    editor_session::EditorWorldSession& session)
    : session_(session)
{
    RegisterHostMethods();
    RebindEditor();
}

EditorSessionRpcHost::~EditorSessionRpcHost()
{
    editor_.reset();
    UnregisterClosedProjectMethods();

    for (const auto& method : hostMethods_)
    {
        static_cast<void>(
            dispatcher_.Unregister(method));
    }
}

std::optional<std::string>
EditorSessionRpcHost::Dispatch(
    const std::string_view payload)
{
    if (BatchContainsWorldSwitch(payload))
    {
        return BatchSwitchError();
    }

    const auto response =
        dispatcher_.Dispatch(payload);

    if (pendingRebind_)
    {
        pendingRebind_ = false;
        RebindEditor();
    }

    return response;
}

rpc::Dispatcher&
EditorSessionRpcHost::Dispatcher() noexcept
{
    return dispatcher_;
}

const rpc::Dispatcher&
EditorSessionRpcHost::Dispatcher() const noexcept
{
    return dispatcher_;
}

EditorRpcService*
EditorSessionRpcHost::Editor() noexcept
{
    return editor_.get();
}

const EditorRpcService*
EditorSessionRpcHost::Editor() const noexcept
{
    return editor_.get();
}

void EditorSessionRpcHost::SetEditorConfigurator(
    std::function<void(EditorRpcService&)> configurator)
{
    editorConfigurator_ =
        std::move(configurator);

    if (editor_ != nullptr &&
        editorConfigurator_)
    {
        editorConfigurator_(*editor_);
    }
}

void EditorSessionRpcHost::AttachViewport(
    ViewportAutomation viewport)
{
    viewportAutomation_ =
        std::move(viewport);

    if (editor_ != nullptr)
    {
        editor_->AttachViewport(
            *viewportAutomation_);
    }
}

void EditorSessionRpcHost::AttachPathRouting(
    PathRoutingAutomation routing)
{
    pathRoutingAutomation_ =
        std::move(routing);

    if (editor_ != nullptr)
    {
        editor_->AttachPathRouting(
            *pathRoutingAutomation_);
    }
}

void EditorSessionRpcHost::AttachPathGeometry(
    PathGeometryAutomation geometry)
{
    pathGeometryAutomation_ =
        std::move(geometry);

    if (editor_ != nullptr)
    {
        editor_->AttachPathGeometry(
            *pathGeometryAutomation_);
    }
}

void EditorSessionRpcHost::AttachBuild(
    BuildAutomation build)
{
    buildAutomation_ =
        std::move(build);

    if (editor_ != nullptr)
    {
        editor_->AttachBuild(
            *buildAutomation_);
    }
}

void EditorSessionRpcHost::PublishEvent(
    std::string type,
    rpc::Value data)
{
    if (editor_ != nullptr)
    {
        editor_->PublishEvent(
            std::move(type),
            std::move(data));
    }
}

std::vector<std::string>
EditorSessionRpcHost::DrainNotifications()
{
    return editor_ != nullptr
        ? editor_->DrainNotifications()
        : std::vector<std::string>{};
}

void EditorSessionRpcHost::RegisterHostMethods()
{
    const auto registerMethod =
        [this](
            rpc::MethodDescriptor descriptor,
            rpc::Dispatcher::MethodHandler handler)
        {
            const std::string name = descriptor.name;
            dispatcher_.Register(
                std::move(descriptor),
                std::move(handler));
            hostMethods_.push_back(name);
        };

    registerMethod(
        {
            .name = "world.active",
            .description =
                "Returns the currently open authoring world and session generation.",
            .mutating = false
        },
        [this](const rpc::Value&)
        {
            if (!session_.HasWorld())
            {
                return rpc::Value(
                    rpc::Value::Object{
                        {"open", false},
                        {
                            "generation",
                            static_cast<i64>(
                                session_.Generation())
                        }
                    });
            }

            rpc::Value result =
                WorldToRpc(
                    session_.ActiveWorld(),
                    session_.Generation());
            result.AsObject().emplace(
                "open",
                true);
            return result;
        });

    registerMethod(
        {
            .name = "world.open",
            .description =
                "Atomically switches the active authoring session to a project world.",
            .mutating = true
        },
        [this](const rpc::Value& params)
        {
            const auto& values =
                RequireParams(params);

            try
            {
                session_.OpenWorld(
                    RequireString(
                        values,
                        "path"));
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(
                    1024,
                    exception.what());
            }

            pendingRebind_ = true;
            rpc::Value result =
                WorldToRpc(
                    session_.ActiveWorld(),
                    session_.Generation());
            result.AsObject().emplace(
                "open",
                true);
            return result;
        });

    registerMethod(
        {
            .name = "world.close",
            .description =
                "Closes the active authoring world while keeping project-level RPC available.",
            .mutating = true
        },
        [this](const rpc::Value&)
        {
            try
            {
                session_.CloseWorld();
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(
                    1024,
                    exception.what());
            }

            pendingRebind_ = true;
            return rpc::Value(
                rpc::Value::Object{
                    {"open", false},
                    {
                        "generation",
                        static_cast<i64>(
                            session_.Generation())
                    }
                });
        });
}

void EditorSessionRpcHost::RegisterClosedProjectMethods()
{
    if (!closedProjectMethods_.empty())
    {
        return;
    }

    auto& project = session_.Project();

    const auto registerMethod =
        [this](
            rpc::MethodDescriptor descriptor,
            rpc::Dispatcher::MethodHandler handler)
        {
            const std::string name = descriptor.name;
            dispatcher_.Register(
                std::move(descriptor),
                std::move(handler));
            closedProjectMethods_.push_back(name);
        };

    registerMethod(
        {
            .name = "project.info",
            .description =
                "Returns the currently open Orbit project.",
            .mutating = false
        },
        [&project](const rpc::Value&)
        {
            return ProjectInfoToRpc(project);
        });

    registerMethod(
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
                for (const auto& world : project.Worlds())
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

            return rpc::Value(std::move(result));
        });

    registerMethod(
        {
            .name = "world.describe",
            .description =
                "Returns one authoritative project world descriptor.",
            .mutating = false
        },
        [&project](const rpc::Value& params)
        {
            const auto& values = RequireParams(params);

            try
            {
                return WorldToRpc(
                    project.DescribeWorld(
                        RequireString(
                            values,
                            "path")));
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(
                    1021,
                    exception.what());
            }
        });

    registerMethod(
        {
            .name = "world.create",
            .description =
                "Creates a versioned project world through ProjectDocument.",
            .mutating = true
        },
        [&project](const rpc::Value& params)
        {
            const auto& values = RequireParams(params);
            const std::string path =
                RequireString(values, "path");
            const std::string displayName =
                RequireString(
                    values,
                    "display_name");

            try
            {
                const auto created =
                    project.CreateWorld(
                        path,
                        displayName);
                return WorldToRpc(
                    project.DescribeWorld(created));
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
        });

    registerMethod(
        {
            .name = "world.set_display_name",
            .description =
                "Changes persistent display metadata for a project world without renaming its file.",
            .mutating = true
        },
        [&project](const rpc::Value& params)
        {
            const auto& values = RequireParams(params);
            const std::string path =
                RequireString(values, "path");
            const std::string displayName =
                RequireString(
                    values,
                    "display_name");

            try
            {
                project.SetWorldDisplayName(
                    path,
                    displayName);
                return WorldToRpc(
                    project.DescribeWorld(path));
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
        });

    registerMethod(
        {
            .name = "world.set_startup",
            .description =
                "Changes the persisted startup world without opening it.",
            .mutating = true
        },
        [&project](const rpc::Value& params)
        {
            const auto& values = RequireParams(params);
            const std::string path =
                RequireString(values, "path");

            try
            {
                project.SetStartupWorld(path);
                return WorldToRpc(
                    project.DescribeWorld(path));
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
        });
}

void EditorSessionRpcHost::UnregisterClosedProjectMethods()
    noexcept
{
    for (const auto& method : closedProjectMethods_)
    {
        static_cast<void>(
            dispatcher_.Unregister(method));
    }

    closedProjectMethods_.clear();
}

void EditorSessionRpcHost::RebindEditor()
{
    editor_.reset();
    UnregisterClosedProjectMethods();

    if (!session_.HasWorld())
    {
        RegisterClosedProjectMethods();
        return;
    }

    try
    {
        editor_ =
            std::make_unique<EditorRpcService>(
                dispatcher_,
                session_.Project(),
                session_.CommandRegistry(),
                session_.Commands(),
                session_.Schemas(),
                session_.Objects(),
                session_.Selection());

        if (editorConfigurator_)
        {
            editorConfigurator_(*editor_);
        }

        if (viewportAutomation_.has_value())
        {
            editor_->AttachViewport(
                *viewportAutomation_);
        }
        if (pathRoutingAutomation_.has_value())
        {
            editor_->AttachPathRouting(
                *pathRoutingAutomation_);
        }
        if (pathGeometryAutomation_.has_value())
        {
            editor_->AttachPathGeometry(
                *pathGeometryAutomation_);
        }
        if (buildAutomation_.has_value())
        {
            editor_->AttachBuild(
                *buildAutomation_);
        }
    }
    catch (...)
    {
        RegisterClosedProjectMethods();
        throw;
    }
}

bool EditorSessionRpcHost::BatchContainsWorldSwitch(
    const std::string_view payload) const noexcept
{
    try
    {
        const rpc::Value document =
            rpc::ParseValue(payload);

        if (!document.IsArray())
        {
            return false;
        }

        for (const auto& request : document.AsArray())
        {
            if (!request.IsObject())
            {
                continue;
            }

            const auto* method =
                request.Find("method");

            if (method != nullptr &&
                method->IsString() &&
                (method->AsString() == "world.open" ||
                 method->AsString() == "world.close"))
            {
                return true;
            }
        }
    }
    catch (...)
    {
        // Let the normal dispatcher produce its parse/validation error.
    }

    return false;
}
} // namespace orbit::editor_rpc
