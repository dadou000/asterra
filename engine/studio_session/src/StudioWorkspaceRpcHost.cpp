#include <orbit/studio_session/StudioWorkspaceRpcHost.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace orbit::studio_session
{
namespace
{
[[nodiscard]] const rpc::Value::Object& RequireObject(
    const rpc::Value& params)
{
    if (!params.IsObject())
    {
        throw rpc::Error(-32602, "Params must be an object.");
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
            std::string(key) + " must be a non-empty string.");
    }

    return found->second.AsString();
}

[[nodiscard]] rpc::Value WorkspaceState(
    const StudioWorkspace& workspace)
{
    rpc::Value::Object result{
        {"open", workspace.HasProject()},
        {
            "generation",
            static_cast<i64>(workspace.Generation())
        }
    };

    if (workspace.HasProject())
    {
        const auto& project = workspace.Project();
        result.emplace(
            "project_id",
            project.Manifest().projectId.ToString());
        result.emplace(
            "name",
            project.Manifest().displayName);
        result.emplace(
            "root",
            project.RootDirectory().generic_string());
        result.emplace(
            "manifest",
            project.ManifestPath().generic_string());
    }

    return rpc::Value(std::move(result));
}
} // namespace

StudioWorkspaceRpcHost::StudioWorkspaceRpcHost(
    StudioWorkspace& workspace)
    : workspace_(workspace)
{
    RegisterMethods();
}

std::optional<std::string>
StudioWorkspaceRpcHost::Dispatch(
    const std::string_view payload)
{
    rpc::Value document;

    try
    {
        document = rpc::ParseValue(payload);
    }
    catch (...)
    {
        // Preserve the canonical JSON-RPC parse error from Dispatcher.
        return dispatcher_.Dispatch(payload);
    }

    if (document.IsArray())
    {
        if (BatchContainsWorkspaceMethod(document))
        {
            return rpc::Serialize(
                rpc::Value(
                    rpc::Value::Object{
                        {"jsonrpc", "2.0"},
                        {"id", rpc::Value{}},
                        {
                            "error",
                            rpc::Value::Object{
                                {"code", i64{1041}},
                                {
                                    "message",
                                    "project.create, project.open, project.close and workspace.active must be dispatched as standalone requests."
                                }
                            }
                        }
                    }));
        }

        if (!workspace_.HasProject())
        {
            return rpc::Serialize(
                rpc::Value(
                    rpc::Value::Object{
                        {"jsonrpc", "2.0"},
                        {"id", rpc::Value{}},
                        {
                            "error",
                            rpc::Value::Object{
                                {"code", i64{1040}},
                                {"message", "Orbit Studio has no open project."}
                            }
                        }
                    }));
        }

        return workspace_.Session().DispatchRpc(payload);
    }

    if (!document.IsObject())
    {
        return dispatcher_.Dispatch(payload);
    }

    const auto* method = document.Find("method");

    if (method != nullptr &&
        method->IsString() &&
        IsWorkspaceMethod(method->AsString()))
    {
        return dispatcher_.Dispatch(payload);
    }

    if (!workspace_.HasProject())
    {
        return NoProjectError(document);
    }

    return workspace_.Session().DispatchRpc(payload);
}

rpc::Dispatcher&
StudioWorkspaceRpcHost::Dispatcher() noexcept
{
    return dispatcher_;
}

const rpc::Dispatcher&
StudioWorkspaceRpcHost::Dispatcher() const noexcept
{
    return dispatcher_;
}

void StudioWorkspaceRpcHost::RegisterMethods()
{
    const auto registerMethod =
        [this](
            rpc::MethodDescriptor descriptor,
            rpc::Dispatcher::MethodHandler handler)
        {
            methods_.push_back(descriptor.name);
            dispatcher_.Register(
                std::move(descriptor),
                std::move(handler));
        };

    registerMethod(
        {
            .name = "workspace.active",
            .description =
                "Returns whether Orbit Studio currently owns an open project.",
            .mutating = false
        },
        [this](const rpc::Value&)
        {
            return WorkspaceState(workspace_);
        });

    registerMethod(
        {
            .name = "project.create",
            .description =
                "Creates and opens a new Orbit project transactionally.",
            .mutating = true
        },
        [this](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);

            try
            {
                workspace_.CreateProject(
                    RequireString(values, "root"),
                    RequireString(values, "name"));
                return WorkspaceState(workspace_);
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(1042, exception.what());
            }
        });

    registerMethod(
        {
            .name = "project.open",
            .description =
                "Opens an Orbit project directory or manifest transactionally.",
            .mutating = true
        },
        [this](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);

            try
            {
                workspace_.OpenProject(
                    RequireString(values, "path"));
                return WorkspaceState(workspace_);
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(1042, exception.what());
            }
        });

    registerMethod(
        {
            .name = "project.close",
            .description =
                "Checkpoints and closes the current Orbit Studio project.",
            .mutating = true
        },
        [this](const rpc::Value&)
        {
            try
            {
                workspace_.CloseProject();
                return WorkspaceState(workspace_);
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(1043, exception.what());
            }
        });
}

bool StudioWorkspaceRpcHost::IsWorkspaceMethod(
    const std::string_view method) const noexcept
{
    for (const auto& name : methods_)
    {
        if (name == method)
        {
            return true;
        }
    }

    return false;
}

bool StudioWorkspaceRpcHost::BatchContainsWorkspaceMethod(
    const rpc::Value& document) const noexcept
{
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

        const auto* method = request.Find("method");

        if (method != nullptr &&
            method->IsString() &&
            IsWorkspaceMethod(method->AsString()))
        {
            return true;
        }
    }

    return false;
}

std::string StudioWorkspaceRpcHost::NoProjectError(
    const rpc::Value& request) const
{
    rpc::Value id;

    if (const auto* requestId = request.Find("id");
        requestId != nullptr)
    {
        id = *requestId;
    }

    return rpc::Serialize(
        rpc::Value(
            rpc::Value::Object{
                {"jsonrpc", "2.0"},
                {"id", std::move(id)},
                {
                    "error",
                    rpc::Value::Object{
                        {"code", i64{1040}},
                        {"message", "Orbit Studio has no open project."}
                    }
                }
            }));
}
} // namespace orbit::studio_session
