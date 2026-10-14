#include <orbit/studio_session/StudioSession.hpp>

#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/path_geometry/PathSource.hpp>
#include <orbit/path_geometry/CorridorSweep.hpp>
#include <orbit/path_geometry/InfrastructureSectionBuilder.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/studio_session/StudioMeshRpc.hpp>
#include <orbit/studio_session/StudioTerrainSampleRpc.hpp>
#include <orbit/studio_session/StudioTerrainStatusRpc.hpp>
#include <orbit/studio_session/ViewportTargetRpc.hpp>
#include <orbit/studio_session/VolumeParticleOutputState.hpp>
#include <orbit/studio_session/VolumeSurfaceEffectState.hpp>
#include <orbit/studio_session/VolumeSurfaceOutputResolver.hpp>
#include <orbit/volume_representation/VolumeOutputRuntime.hpp>

#include <algorithm>
#include <cmath>
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
    VolumeParticleOutputs().Clear();
    VolumeSurfaceOutputs().Clear();
    VolumeSurfaceEffects().Clear();
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

    // Particle output is transferred exactly once into a frame packet. M38
    // deliberately preserves transport data without inventing lifetime,
    // gravity, collision or visual policy that is not authored yet.
    VolumeParticleOutputs().Consume(
        world.Objects(),
        world.Universe(),
        world.Surfaces(),
        particles.Drain());

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

    // Resolved deposits feed a transient body-local material influence layer.
    // This deliberately does not mutate authored terrain: rendering, material
    // physics and later persistence systems consume the sampled runtime state.
    VolumeSurfaceEffects().Advance(
        world.Universe().Bodies(),
        VolumeSurfaceOutputs().Resolved(),
        runtime.Diagnostics().deltaSeconds);
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
      terrainBake_(world_),
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
    RegisterStudioTerrainStatusRpc(
        rpc_.Dispatcher(),
        *this);
    RegisterStudioTerrainSampleRpc(
        rpc_.Dispatcher(),
        *this);
    RegisterStudioMeshRpc(
        rpc_.Dispatcher(),
        *this);
    rpc_.Dispatcher().Register(
        {.name = "infrastructure.build_status",
         .description = "Read cached corridor build state and errors.",
         .mutating = false},
        [this](const rpc::Value& params) -> rpc::Value
        {
            if (!params.IsObject()) throw rpc::Error(-32602, "Object params required.");
            const auto* idValue = params.Find("edge");
            if (!idValue || !idValue->IsString())
                throw rpc::Error(-32602, "edge must be a path edge ID string.");
            const auto edge = scene::ObjectId::Parse(idValue->AsString());
            if (!edge) throw rpc::Error(-32602, "Invalid edge ID.");
            const auto* product = pathProducts_.Find(*edge);
            const bool dirty = pathProducts_.IsDirty(*edge);
            const std::string error = pathProducts_.RebuildError(*edge);
            return rpc::Value(rpc::Value::Object{
                {"edge", edge->ToString()},
                {"state", !error.empty() ? "failed" : dirty ? "dirty" :
                          product ? "ready" : "missing"},
                {"has_last_valid_mesh", product != nullptr},
                {"error", error},
                {"build_signature", product ? std::to_string(product->buildSignature) : std::string{}},
                {"dirty_count", static_cast<i64>(pathProducts_.DirtyCount())}
            });
        });
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

std::vector<terrain::PhysicalTerrainPageAddress>
StudioSession::ConstructionPhysicalPages(
    const universe::BodyId body, const std::size_t maximumPages,
    const math::Double3* observerDirection,
    const std::string_view viewportId) const
{
    if (viewportId.empty())
        return corridorConstruction_.PagesForBody(
            body, maximumPages, observerDirection);

    auto& selection = constructionSelections_[std::string(viewportId)];
    if (selection.body != body ||
        selection.worldGeneration != world_.Generation())
    {
        selection = {};
        selection.body = body;
        selection.worldGeneration = world_.Generation();
    }
    auto result = corridorConstruction_.PagesForBody(
        body, maximumPages, observerDirection, selection.pages);
    selection.pages = result;
    return result;
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

std::size_t StudioSession::QueueInfrastructureCorridorTerrain(
    const path_geometry::PathDerivedProduct& product,
    const universe::BodyId body,
    const f64 influenceRadiusMeters,
    const std::size_t maximumPages)
{
    if (!world_.HasWorld() || !body || maximumPages == 0U ||
        !std::isfinite(influenceRadiusMeters) || influenceRadiusMeters < 0.0 ||
        product.stations.size() < 2U || !product.frame)
        throw std::invalid_argument("Invalid corridor construction mapping inputs.");

    const auto* targetBody = world_.Universe().Bodies().FindBody(body);
    if (!targetBody)
        throw std::invalid_argument("Construction target body not found.");

    const auto runtimeCatalog = terrainRuntime_.Catalog();
    const StudioTerrainViewportRuntimeSnapshot* runtime = nullptr;
    for (const auto& entry : runtimeCatalog)
    {
        if (entry.body != body) continue;
        if (runtime != nullptr && (runtime->planet.id != entry.planet.id ||
                                    runtime->physicalPageLevel != entry.physicalPageLevel))
            throw std::invalid_argument("Ambiguous physical terrain mapping for body.");
        runtime = &entry;
    }
    if (!runtime || !runtime->planet.id)
        throw std::invalid_argument("No active physical terrain runtime for construction body.");

    std::vector<math::Double3> bodyPositions;
    bodyPositions.reserve(product.stations.size());
    for (const auto& station : product.stations)
    {
        const auto transformed = world_.Universe().Frames().TransformPoint(
            {.frame = product.frame, .localMeters = station.position},
            targetBody->frame, clock_.Time());
        if (!transformed)
            throw std::invalid_argument("Corridor frame cannot resolve to target body.");
        bodyPositions.push_back(transformed->localMeters);
    }
    const auto pages = path_geometry::ProjectCorridorToTerrainPages(
        runtime->planet, bodyPositions, influenceRadiusMeters,
        runtime->physicalPageLevel, maximumPages);
    if (!pages)
        throw std::invalid_argument("Corridor planetary page projection exceeded safe limits.");
    return QueueInfrastructureTerrainPages(*pages, maximumPages);
}

std::size_t StudioSession::QueueInfrastructureTerrainPages(
    const std::span<const terrain::PhysicalTerrainPageAddress> pages,
    const std::size_t maximumPages)
{
    if (pages.size() > maximumPages || maximumPages == 0U)
        throw std::invalid_argument(
            "Infrastructure page invalidation exceeds bounded budget.");
    // Validate all inputs first so invalid requests cannot partially enqueue.
    for (const auto& page : pages)
    {
        if (!page.planet || page.tile.level > 24U ||
            page.tile.x >= (1U << page.tile.level) ||
            page.tile.y >= (1U << page.tile.level))
            throw std::invalid_argument(
                "Infrastructure physical page address is invalid.");
    }
    const auto initial = pendingTerrainInvalidations_.size();
    for (const auto& page : pages)
    {
        QueueTerrainInvalidation({
            .kind = terrain_dependency::TerrainChangeKind::TerrainAuthoring,
            .scope = {
                .planet = page.planet,
                .global = false,
                .center = page.tile,
                .radiusTiles = 0U,
                .downstreamRadiusTiles = 0U
            }
        });
    }
    return pendingTerrainInvalidations_.size() - initial;
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
    corridorConstruction_.Clear();
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

    // world.open and world.close over RPC swap the world state (and with it
    // every per-body GPU terrain cache) without passing through OpenWorld /
    // CloseWorld above, which are what clear the physical page service on the
    // UI path. Do the same here, before anything can use a runtime that still
    // points at a freed cache or a queued change that belongs to the old world.
    if (world_.Generation() !=
        terrainPagesWorldGeneration_)
    {
        terrainPagesWorldGeneration_ =
            world_.Generation();
        pendingTerrainInvalidations_.clear();
        terrainPhysicalPages_.Clear();
    }

    if (world_.HasWorld())
    {
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

    const auto rpcTerrainBindings = terrainRuntime_.Catalog();
    std::vector<terrain::PhysicalTerrainPageAddress> rpcConstructionPages;
    for (const auto& binding : rpcTerrainBindings)
    {
        const auto direction = math::Normalize(binding.observer.meters);
        const auto pages = ConstructionPhysicalPages(binding.body, 8U,
            &direction, binding.viewportId);
        rpcConstructionPages.insert(rpcConstructionPages.end(),
            pages.begin(), pages.end());
    }
    terrainPhysicalPages_.Sync(
        rpcTerrainBindings, clock_.Time(), rpcConstructionPages);
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

StudioTerrainBakeController& StudioSession::TerrainBake() noexcept
{
    return terrainBake_;
}

const StudioTerrainBakeController& StudioSession::TerrainBake() const noexcept
{
    return terrainBake_;
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
        terrainBake_.Clear();
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

    result.compositionChanged =
        world_.RefreshUniverseIfChanged();

    // Bake before anything reads the terrain: the first bake of a planet is
    // blocking, so no page is generated from the plate model, and a finished
    // rebake swaps in here by recomposing the terrain source.
    if (terrainBake_.Tick())
    {
        result.compositionChanged = true;
    }

    result.activeBodyChanged =
        activeBody_.Refresh();
    result.pathNetworkRebound =
        pathNetwork_.RefreshBinding();
    result.pathRoutingRebound =
        pathRouting_.RefreshBinding();
    result.pathProductsInvalidated =
        pathProducts_.RefreshBinding();
    static_cast<void>(pathProducts_.RefreshSemanticInvalidation());
    // Discover only newly authored infrastructure edges. Reuse scene revision
    // to avoid rescanning unrelated world objects during steady-state ticks.
    const u64 sceneRevision = world_.Objects().Revision();
    if (infrastructureDiscoveryWorldGeneration_ != world_.Generation() ||
        infrastructureDiscoveryRevision_ != sceneRevision)
    {
        infrastructureDiscoveryWorldGeneration_ = world_.Generation();
        infrastructureDiscoveryRevision_ = sceneRevision;
        const auto roots = world_.Objects().Roots();
        for (const auto& root : roots)
        {
            // Iterative traversal; path networks may be nested under world folders.
            std::vector<scene::ObjectId> pending{root.id};
            while (!pending.empty())
            {
                const auto parent = pending.back();
                pending.pop_back();
                for (const auto& child : world_.Objects().Children(parent))
                {
                    if (child.type == paths::kPathEdgeType)
                    {
                        const auto edge = pathNetwork_.Service().FindEdge(child.id);
                        if (edge && !edge->infrastructureCatalogId.empty() &&
                            pathProducts_.Find(edge->id) == nullptr &&
                            !pathProducts_.IsDirty(edge->id))
                            pathProducts_.MarkDirty(edge->id);
                    }
                    else if (child.type != paths::kPathNodeType)
                    {
                        pending.push_back(child.id);
                    }
                }
            }
        }
    }
    // Bounded initial production resolver for authored *frame-local* road/rail
    // edges. Unsupported categories remain dirty with an inspectable failure;
    // no silent frame guesses for moving decks, geodetic anchors or routing.
    static_cast<void>(pathProducts_.RebuildDirty(
        [this](const scene::ObjectId id)
            -> std::optional<UniverseBoundPathCache::RebuildRequest>
        {
            const auto edge = pathNetwork_.Service().FindEdge(id);
            if (!edge || edge->infrastructureCatalogId.empty()) return std::nullopt;
            const auto a = pathNetwork_.Service().FindNode(edge->startNode);
            const auto b = pathNetwork_.Service().FindNode(edge->endNode);
            if (!a || !b) return std::nullopt;
            const auto* fa = std::get_if<paths::FramePointAnchor>(&a->anchor);
            const auto* fb = std::get_if<paths::FramePointAnchor>(&b->anchor);
            if (!fa || !fb || fa->frame != fb->frame)
                return std::nullopt;
            if (!world_.Universe().Frames().Contains(fa->frame))
                return std::nullopt;
            if (edge->mode == paths::EdgeMode::Routed)
                return std::nullopt;
            const bool isRoad = edge->infrastructureCatalogId == "rural-road";
            const bool isRail = edge->infrastructureCatalogId == "ballasted-rail";
            if ((!isRoad && !isRail) || edge->infrastructureCatalogVersion != 1)
                return std::nullopt;
            paths::PathProfile profile{};
            profile.name = isRail ? "Ballasted Rail" : "Rural Road";
            profile.kind = isRail ? paths::PathProfileKind::Rail : paths::PathProfileKind::Road;
            // Formation includes both railheads plus the ballast margins.
            // Width is the usable pavement for a road and formation for rail.
            profile.widthMeters = isRail ? 4.035 : 7.0;
            profile.lanes = isRail ? 1U : 2U;
            // Advisory catalog limits; the user can later supply custom
            // engineering profile constraints through the profile system.
            profile.maximumGrade = isRail ? 0.035 : 0.10;
            profile.minimumRadiusMeters = isRail ? 150.0 : 0.0;
            path_geometry::PathSourceRequest source{};
            source.edge = *edge;
            source.startNode = *a;
            source.endNode = *b;
            source.targetFrame = fa->frame;
            source.frames = &world_.Universe().Frames();
            source.bodies = &world_.Universe().Bodies();
            source.atTime = clock_.Time();
            source.sourceRevision = world_.Objects().Revision();
            const auto centerline = path_geometry::BuildPathCenterline(source);
            if (!centerline) return std::nullopt;
            // Domain recipe, not another hand-authored Studio-specific
            // triangle set. Curvature and design speed can generate bounded
            // railway cant in the same station frame as the swept railhead.
            const auto planned = path_geometry::PlanInfrastructureSection(
                *centerline, profile, {},
                edge->designSpeedKmh > 0.0 ?
                    edge->designSpeedKmh / 3.6 : 0.0,
                edge->easeTurns);
            UniverseBoundPathCache::RebuildRequest request{};
            request.authoredSignature = world_.Objects().Revision();
            if (request.authoredSignature == 0U) request.authoredSignature = 1U;
            request.centerline = *centerline;
            request.profile = std::move(profile);
            request.corridor = planned.section;
            return request;
        }, 2U));
    result.viewportTargetsChanged =
        viewports_.Refresh();

    RefreshTerrainDebugGeneration();

    result.terrainRuntimeChanged =
        terrainRuntime_.Refresh();

    const auto terrainBindings = terrainRuntime_.Catalog();
    std::vector<terrain::PhysicalTerrainPageAddress> constructionPages;
    for (const auto& binding : terrainBindings)
    {
        const auto direction = math::Normalize(binding.observer.meters);
        const auto pages = ConstructionPhysicalPages(binding.body, 8U,
            &direction);
        constructionPages.insert(constructionPages.end(),
            pages.begin(), pages.end());
    }
    terrainPhysicalPages_.Sync(
        terrainBindings, clock_.Time(), constructionPages);

    // Publish only body-fixed corridors. Moving-deck/inertial alignments
    // require dynamic construction semantics and must not excavate a planet.
    // Registry revisions suppress rebuilds on steady-state ticks.
    const auto corridorProducts = pathProducts_.Products();
    for (const auto& binding : terrainBindings)
    {
        const auto* body = world_.Universe().Bodies().FindBody(binding.body);
        if (!body || !terrainRuntime_.IsCurrent(binding)) continue;
        std::vector<CorridorConstructionEntry> entries;
        for (const auto* product : corridorProducts)
        {
            // Dirty means an authoritative edit is waiting for its derived
            // replacement. The path cache intentionally retains the
            // last-valid product, so keep its terrain cut until a successful
            // rebuild replaces it. Only a truly deleted/unsupported edge
            // should remove its previous footprint.
            if (!product || product->frame != body->frame ||
                product->stations.size() < 2U) continue;
            const auto edge = pathNetwork_.Service().FindEdge(product->edge);
            if (!edge || edge->infrastructureCatalogVersion != 1U) continue;
            const bool road = edge->infrastructureCatalogId == "rural-road";
            const bool rail = edge->infrastructureCatalogId == "ballasted-rail";
            if (!road && !rail) continue;
            // Elevated supports may contact terrain at isolated foundations,
            // but their decks must never grade the ground continuously.
            // Tunnels need volume excavation, not heightfield flattening.
            if (edge->constructionMode != "terrain") continue;
            CorridorConstructionEntry entry{};
            entry.layer.edge = product->edge;
            entry.layer.stations = product->stations;
            entry.layer.footprint = {
                .halfWidthMeters = product->widthMeters * 0.5,
                .shoulderMeters = road ? 0.75 : 0.60,
                .blendMeters = 3.0
            };
            entry.revision = product->buildSignature != 0U ?
                product->buildSignature : 1U;
            entries.push_back(std::move(entry));
        }
        // Empty snapshot matters: deleting the last corridor must restore
        // its old construction footprint.
        try
        {
            static_cast<void>(corridorConstruction_.Publish(
                binding.body, binding.planet,
                terrainRuntime_.TerrainSource(binding),
                binding.physicalPageLevel, std::move(entries),
                terrainPhysicalPages_));
        }
        catch (const std::invalid_argument&)
        {
            // Unsupported/oversized mapping is not a reason to issue a
            // global terrain invalidation. Keep the previous publication.
        }
    }
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
