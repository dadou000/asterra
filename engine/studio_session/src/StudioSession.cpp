#include <orbit/studio_session/StudioSession.hpp>

#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_session/ViewportTargetRpc.hpp>

#include <stdexcept>
#include <utility>

namespace orbit::studio_session
{
namespace
{
[[nodiscard]] std::string LifecycleError(
    const rpc::Value& response)
{
    const auto* error = response.Find("error");

    if (error == nullptr || !error->IsObject())
    {
        return {};
    }

    if (const auto* message = error->Find("message");
        message != nullptr && message->IsString())
    {
        return message->AsString();
    }

    return "Studio world lifecycle RPC failed.";
}
} // namespace

StudioSession::StudioSession(
    documents::ProjectDocument& project)
    : world_(project),
      documents_(world_),
      activeBody_(world_),
      viewports_(world_, activeBody_),
      pathNetwork_(world_),
      pathRouting_(world_),
      pathProducts_(world_),
      rpc_(world_)
{
    RegisterViewportTargetRpc(
        rpc_.Dispatcher(),
        viewports_);
    static_cast<void>(activeBody_.Refresh());
    static_cast<void>(pathNetwork_.RefreshBinding());
    static_cast<void>(pathRouting_.RefreshBinding());
    static_cast<void>(pathProducts_.RefreshBinding());
}

editor_session::EditorWorldSession&
StudioSession::World() noexcept
{
    return world_;
}

const editor_session::EditorWorldSession&
StudioSession::World() const noexcept
{
    return world_;
}

editor_session::ActiveBodyModel&
StudioSession::ActiveBody() noexcept
{
    return activeBody_;
}

const editor_session::ActiveBodyModel&
StudioSession::ActiveBody() const noexcept
{
    return activeBody_;
}

ViewportTargetRegistry&
StudioSession::Viewports() noexcept
{
    return viewports_;
}

const ViewportTargetRegistry&
StudioSession::Viewports() const noexcept
{
    return viewports_;
}

WorldBoundPathNetwork&
StudioSession::PathNetwork() noexcept
{
    return pathNetwork_;
}

const WorldBoundPathNetwork&
StudioSession::PathNetwork() const noexcept
{
    return pathNetwork_;
}

UniverseBoundRoutePlanner&
StudioSession::PathRouting() noexcept
{
    return pathRouting_;
}

const UniverseBoundRoutePlanner&
StudioSession::PathRouting() const noexcept
{
    return pathRouting_;
}

UniverseBoundPathCache&
StudioSession::PathProducts() noexcept
{
    return pathProducts_;
}

const UniverseBoundPathCache&
StudioSession::PathProducts() const noexcept
{
    return pathProducts_;
}

std::vector<editor_session::WorldDocumentItem>
StudioSession::Worlds() const
{
    return documents_.Catalog();
}

std::optional<editor_session::WorldDocumentItem>
StudioSession::ActiveWorld() const
{
    return documents_.Active();
}

documents::WorldDescriptor
StudioSession::CreateWorld(
    const std::filesystem::path& relativePath,
    const std::string_view displayName)
{
    return documents_.Create(
        relativePath,
        displayName);
}

documents::WorldDescriptor
StudioSession::RenameWorld(
    const std::filesystem::path& relativePath,
    const std::string_view displayName)
{
    return documents_.Rename(
        relativePath,
        displayName);
}

documents::WorldDescriptor
StudioSession::SetStartupWorld(
    const std::filesystem::path& relativePath)
{
    return documents_.SetStartup(
        relativePath);
}

void StudioSession::OpenWorld(
    const std::filesystem::path& relativePath)
{
    DispatchWorldLifecycle(
        "world.open",
        relativePath);
}

void StudioSession::CloseWorld()
{
    DispatchWorldLifecycle(
        "world.close",
        std::nullopt);
}

std::optional<std::string>
StudioSession::DispatchRpc(
    const std::string_view payload)
{
    auto response = rpc_.Dispatch(payload);

    if (world_.HasWorld())
    {
        static_cast<void>(activeBody_.Refresh());
    }
    else
    {
        activeBody_.Clear();
    }

    static_cast<void>(pathNetwork_.RefreshBinding());
    static_cast<void>(pathRouting_.RefreshBinding());
    static_cast<void>(pathProducts_.RefreshBinding());
    static_cast<void>(viewports_.Refresh());
    return response;
}

editor_rpc::EditorSessionRpcHost&
StudioSession::Rpc() noexcept
{
    return rpc_;
}

const editor_rpc::EditorSessionRpcHost&
StudioSession::Rpc() const noexcept
{
    return rpc_;
}

StudioTickResult StudioSession::Tick(
    const bool pollPlugins)
{
    StudioTickResult result{
        .worldGeneration = world_.Generation(),
        .universeGeneration = world_.UniverseGeneration()
    };

    if (!world_.HasWorld())
    {
        const bool hadBody =
            activeBody_.Active().has_value();
        activeBody_.Clear();
        result.activeBodyChanged = hadBody;
        result.pathNetworkRebound =
            pathNetwork_.RefreshBinding();
        result.pathRoutingRebound =
            pathRouting_.RefreshBinding();
        result.pathProductsInvalidated =
            pathProducts_.RefreshBinding();
        result.viewportTargetsChanged =
            viewports_.Refresh();
        result.worldGeneration =
            world_.Generation();
        result.universeGeneration =
            world_.UniverseGeneration();
        return result;
    }

    if (pollPlugins)
    {
        result.pluginsReloaded =
            world_.Plugins().PollHotReload();
    }
    result.activeBodyChanged =
        activeBody_.Refresh();
    result.pathNetworkRebound =
        pathNetwork_.RefreshBinding();
    result.pathRoutingRebound =
        pathRouting_.RefreshBinding();
    result.pathProductsInvalidated =
        pathProducts_.RefreshBinding();
    result.viewportTargetsChanged =
        viewports_.Refresh();
    result.worldGeneration =
        world_.Generation();
    result.universeGeneration =
        world_.UniverseGeneration();
    return result;
}

void StudioSession::DispatchWorldLifecycle(
    const std::string_view method,
    const std::optional<std::filesystem::path> path)
{
    rpc::Value::Object params;

    if (path.has_value())
    {
        params.emplace(
            "path",
            path->generic_string());
    }

    const rpc::Value request(
        rpc::Value::Object{
            {"jsonrpc", "2.0"},
            {"id", "studio-session"},
            {"method", std::string(method)},
            {"params", std::move(params)}
        });

    const auto responseText =
        DispatchRpc(
            rpc::Serialize(request));

    if (!responseText.has_value())
    {
        throw std::runtime_error(
            "Studio world lifecycle RPC returned no response.");
    }

    const auto response =
        rpc::ParseValue(*responseText);
    const std::string error =
        LifecycleError(response);

    if (!error.empty())
    {
        throw std::runtime_error(error);
    }
}
} // namespace orbit::studio_session
