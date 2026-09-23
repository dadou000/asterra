#include <orbit/studio_session/StudioSession.hpp>

#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_session/ViewportTargetRpc.hpp>
#include <orbit/studio_session/VolumeSurfaceOutputResolver.hpp>
#include <orbit/volume_representation/VolumeOutputRuntime.hpp>

#include <algorithm>
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

void ResetVolumeOutputRuntime() noexcept
{
    volume_representation::
        VolumeOutputRuntimeService().Reset();
    volume_representation::
        VolumeParticleRequests().Clear();
    volume_representation::
        VolumeSurfaceRequests().Clear();
    VolumeSurfaceOutputs().Clear();
}

void TickVolumeOutputRuntime(
    editor_session::EditorWorldSession& world,
    const time::SimulationTime atTime)
{
    auto& particles =
        volume_representation::
            VolumeParticleRequests();
    auto& surfaces =
        volume_representation::
            VolumeSurfaceRequests();

    particles.Clear();
    surfaces.Clear();

    auto& runtime =
        volume_representation::
            VolumeOutputRuntimeService();
    runtime.SetParticleSink(&particles);
    runtime.SetSurfaceSink(&surfaces);

    static_cast<void>(
        runtime.TickWorld(
            world.Objects(),
            atTime));

    // Surface output has a real authoritative consumer at M38. Transfer the
    // raw batch exactly once, resolve it against the owning composed body and
    // Surface capability, and retain only resolved body-local deposits.
    const auto surfaceBatch =
        surfaces.Drain();

    VolumeSurfaceOutputs().Resolve(
        world.Objects(),
        world.Universe(),
        world.Surfaces(),
        surfaceBatch);
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
      terrainPhysicalPages_(
          world_,
          terrainDebugPages_),
      terrainRuntime_(
          world_,
          viewports_,
          terrainDebugPages_),
      rpc_(world_)
{
    RegisterViewportTargetRpc(
        rpc_.Dispatcher(),
        viewports_);
    static_cast<void>(activeBody_.Refresh());
    static_cast<void>(pathNetwork_.RefreshBinding());
    static_cast<void>(pathRouting_.RefreshBinding());
    static_cast<void>(pathProducts_.RefreshBinding());
    RefreshTerrainDebugGeneration();
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

terrain_debug::TerrainDebugLivePages&
StudioSession::TerrainDebugPages() noexcept
{
    return terrainDebugPages_;
}

const terrain_debug::TerrainDebugLivePages&
StudioSession::TerrainDebugPages() const noexcept
{
    return terrainDebugPages_;
}

StudioTerrainPhysicalPageService&
StudioSession::TerrainPhysicalPages() noexcept
{
    return terrainPhysicalPages_;
}

const StudioTerrainPhysicalPageService&
StudioSession::TerrainPhysicalPages() const noexcept
{
    return terrainPhysicalPages_;
}

StudioTerrainRuntimeBridge&
StudioSession::TerrainRuntime() noexcept
{
    return terrainRuntime_;
}

const StudioTerrainRuntimeBridge&
StudioSession::TerrainRuntime() const noexcept
{
    return terrainRuntime_;
}

StudioTerrainPerformanceDiagnostics&
StudioSession::TerrainPerformance() noexcept
{
    return terrainPerformance_;
}

const StudioTerrainPerformanceDiagnostics&
StudioSession::TerrainPerformance() const noexcept
{
    return terrainPerformance_;
}

SimulationClock&
StudioSession::Clock() noexcept
{
    return clock_;
}

const SimulationClock&
StudioSession::Clock() const noexcept
{
    return clock_;
}

void StudioSession::QueueTerrainInvalidation(
    const terrain_dependency::TerrainInvalidationRequest& request)
{
    if (!request.scope.IsValid())
    {
        throw std::invalid_argument(
            "Studio terrain invalidation scope is invalid.");
    }

    const auto sameRequest =
        [&](const terrain_dependency::TerrainInvalidationRequest& queued)
        {
            return
                queued.kind == request.kind &&
                queued.scope.planet == request.scope.planet &&
                queued.scope.global == request.scope.global &&
                queued.scope.center == request.scope.center &&
                queued.scope.radiusTiles == request.scope.radiusTiles &&
                queued.scope.downstreamRadiusTiles ==
                    request.scope.downstreamRadiusTiles;
        };

    if (std::find_if(
            pendingTerrainInvalidations_.begin(),
            pendingTerrainInvalidations_.end(),
            sameRequest) ==
        pendingTerrainInvalidations_.end())
    {
        pendingTerrainInvalidations_.push_back(request);
    }
}

void StudioSession::QueueTerrainInvalidations(
    const std::span<
        const terrain_dependency::TerrainInvalidationRequest>
        requests)
{
    for (const auto& request : requests)
    {
        QueueTerrainInvalidation(request);
    }
}

std::span<
    const terrain_dependency::TerrainInvalidationRequest>
StudioSession::PendingTerrainInvalidations() const noexcept
{
    return pendingTerrainInvalidations_;
}

std::vector<
    terrain_dependency::TerrainInvalidationRequest>
StudioSession::TakeTerrainInvalidations()
{
    return std::exchange(
        pendingTerrainInvalidations_,
        {});
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
    pendingTerrainInvalidations_.clear();
    terrainPhysicalPages_.Clear();
    ResetVolumeOutputRuntime();

    DispatchWorldLifecycle(
        "world.open",
        relativePath);
}

void StudioSession::CloseWorld()
{
    DispatchWorldLifecycle(
        "world.close",
        std::nullopt);
    pendingTerrainInvalidations_.clear();
    terrainPhysicalPages_.Clear();
    ResetVolumeOutputRuntime();
}

std::optional<std::string>
StudioSession::DispatchRpc(
    const std::string_view payload)
{
    auto response = rpc_.Dispatch(payload);

    if (world_.HasWorld())
    {
        // Semantic edits must compose before any generation-bound consumer
        // refreshes. ActiveBodyModel still has its own defensive refresh, but
        // Studio's lifecycle no longer relies on that side effect.
        static_cast<void>(
            world_.RefreshUniverseIfChanged());
        static_cast<void>(
            activeBody_.Refresh());
    }
    else
    {
        activeBody_.Clear();
    }

    static_cast<void>(pathNetwork_.RefreshBinding());
    static_cast<void>(pathRouting_.RefreshBinding());
    static_cast<void>(pathProducts_.RefreshBinding());
    static_cast<void>(viewports_.Refresh());
    RefreshTerrainDebugGeneration();
    static_cast<void>(
        terrainRuntime_.Refresh());

    terrainPhysicalPages_.Sync(
        terrainRuntime_.Catalog());
    terrainPhysicalPages_.QueueChanges(
        TakeTerrainInvalidations());
    terrainPhysicalPages_.Tick();

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
        RefreshTerrainDebugGeneration();
        result.terrainRuntimeChanged =
            terrainRuntime_.Refresh();
        terrainPhysicalPages_.Clear();
        pendingTerrainInvalidations_.clear();
        ResetVolumeOutputRuntime();
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

    // Composition is the first runtime-bound refresh after all semantic
    // mutation sources for this tick. Everything below observes the new
    // UniverseComposition/SurfaceComposition generation or the unchanged
    // previous generation.
    result.compositionChanged =
        world_.RefreshUniverseIfChanged();

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

    RefreshTerrainDebugGeneration();

    result.terrainRuntimeChanged =
        terrainRuntime_.Refresh();

    terrainPhysicalPages_.Sync(
        terrainRuntime_.Catalog());
    terrainPhysicalPages_.QueueChanges(
        TakeTerrainInvalidations());
    terrainPhysicalPages_.Tick();

    TickVolumeOutputRuntime(
        world_,
        clock_.Time());

    result.worldGeneration =
        world_.Generation();
    result.universeGeneration =
        world_.UniverseGeneration();
    return result;
}

void StudioSession::RefreshTerrainDebugGeneration()
{
    const u64 generation =
        world_.UniverseGeneration();

    if (generation ==
        terrainDebugUniverseGeneration_)
    {
        return;
    }

    terrainDebugPages_.Clear();
    terrainDebugUniverseGeneration_ =
        generation;
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
