#include <orbit/studio_ui/StudioRenderViewSet.hpp>

#include <orbit/studio_ui/StudioViewportCamera.hpp>
#include <orbit/terrain/TerrainSource.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <utility>

namespace orbit::studio_ui
{
StudioRenderViewSet::StudioRenderViewSet(
    rhi::Device& device,
    studio_session::StudioSession& session) noexcept
    : device_(&device),
      session_(&session)
{
}

StudioRenderViewSet::~StudioRenderViewSet() = default;

void StudioRenderViewSet::CreateDefaults()
{
    if (!views_.contains("studio.primary"))
    {
        Create(
            "studio.primary",
            studio_session::ViewportMode::Perspective,
            true,
            {
                .width = 960,
                .height = 640
            });
    }

    if (!views_.contains("studio.map"))
    {
        Create(
            "studio.map",
            studio_session::ViewportMode::BodyMap,
            true,
            {
                .width = 640,
                .height = 480
            });
    }
}

void StudioRenderViewSet::Create(
    std::string id,
    const studio_session::ViewportMode mode,
    const bool followActiveBody,
    render_view::RenderViewDesc desc)
{
    if (device_ == nullptr || session_ == nullptr)
    {
        throw std::logic_error(
            "Studio render-view set has no device/session binding.");
    }

    if (id.empty())
    {
        throw std::invalid_argument(
            "Studio render-view ID must not be empty.");
    }

    if (views_.contains(id) ||
        session_->Viewports().Find(id) != nullptr)
    {
        throw std::invalid_argument(
            "Studio render-view ID is already in use.");
    }

    desc.width = std::max(desc.width, 1U);
    desc.height = std::max(desc.height, 1U);

    const std::string targetId = id;
    session_->Viewports().Register(
        targetId,
        mode,
        followActiveBody);

    try
    {
        navigationStates_.emplace(
            targetId,
            StudioTerrainNavigationState{});
        compositionEnabled_.emplace(targetId, true);
        debugFields_.emplace(
            targetId,
            terrain_debug::TerrainDebugField::Uplift);
        debugPhysicalPageLevels_.emplace(
            targetId,
            static_cast<u8>(8));
        terrainDiagnosticOverlays_.emplace(
            targetId,
            StudioTerrainDiagnosticOverlayOptions{});

        auto view =
            std::make_unique<render_view::RenderView>(
                *device_,
                desc);
        views_.emplace(
            std::move(id),
            std::move(view));
    }
    catch (...)
    {
        navigationStates_.erase(targetId);
        compositionEnabled_.erase(targetId);
        debugFields_.erase(targetId);
        debugPhysicalPageLevels_.erase(targetId);
        debugPhysicalPages_.erase(targetId);
        terrainSurfacePicks_.erase(targetId);
        terrainAuthoringOverlays_.erase(targetId);
        terrainDiagnosticOverlays_.erase(targetId);
        liveDebugPages_.erase(targetId);
        static_cast<void>(
            session_->Viewports().Unregister(
                targetId));
        throw;
    }
}

bool StudioRenderViewSet::Destroy(
    const std::string_view id) noexcept
{
    const auto found = views_.find(id);

    if (found == views_.end())
    {
        return false;
    }

    const std::string ownedId = found->first;
    views_.erase(found);
    navigationStates_.erase(ownedId);
    compositionEnabled_.erase(ownedId);
    textDiagnosticsHud_.erase(ownedId);
    orbitalPatchStats_.erase(ownedId);
    clipmapPlanStats_.erase(ownedId);
    terrainLayers_.erase(ownedId);
    debugFields_.erase(ownedId);
    debugPhysicalPageLevels_.erase(ownedId);
    debugPhysicalPages_.erase(ownedId);
    terrainSurfacePicks_.erase(ownedId);
    terrainAuthoringOverlays_.erase(ownedId);
    terrainDiagnosticOverlays_.erase(ownedId);
    liveDebugPages_.erase(ownedId);

    if (session_ != nullptr)
    {
        static_cast<void>(
            session_->Viewports().Unregister(ownedId));
    }

    return true;
}

void StudioRenderViewSet::Resize(
    const std::string_view id,
    const u32 width,
    const u32 height)
{
    auto* view = Find(id);

    if (view == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    view->Resize(
        std::max(width, 1U),
        std::max(height, 1U));
}

void StudioRenderViewSet::SetCompositionEnabled(
    const std::string_view id,
    const bool enabled)
{
    const auto found = compositionEnabled_.find(id);
    if (found == compositionEnabled_.end())
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }
    found->second = enabled;
}

bool StudioRenderViewSet::CompositionEnabled(
    const std::string_view id) const noexcept
{
    const auto found = compositionEnabled_.find(id);
    return found != compositionEnabled_.end() && found->second;
}

void StudioRenderViewSet::SetNavigationSpeedScale(
    const std::string_view id,
    const f64 scale)
{
    if (!std::isfinite(scale) ||
        scale <= 0.0)
    {
        throw std::invalid_argument(
            "Studio viewport navigation speed scale must be finite and positive.");
    }

    auto& state =
        RequireNavigationState(id);

    state.config.movementSpeedScale =
        std::clamp(
            scale,
            0.01,
            1'000.0);
}

f64 StudioRenderViewSet::NavigationSpeedScale(
    const std::string_view id) const
{
    return
        RequireNavigationState(id).
            config.
            movementSpeedScale;
}

namespace
{
// A body with no authored terrain is treated as a smooth sphere so it can use
// the same navigation as terrain bodies.
class ReferenceSphereSource final : public terrain::TerrainSource
{
public:
    [[nodiscard]] terrain::TerrainSample Sample(
        const terrain::TerrainQuery&) const noexcept override
    {
        return {};
    }
};

[[nodiscard]] world::PlanetDefinition ReferencePlanet(
    studio_session::StudioSession& session,
    const editor_session::ActiveBodyTarget& target)
{
    if (auto planet =
            session.World().Surfaces().Registry().
                SphericalPlanetDefinition(target.body);
        planet.has_value())
    {
        return *planet;
    }

    return {
        .radiusMeters = std::max(target.referenceRadiusMeters, 1.0),
        .id = {
            .high = target.body.high,
            .low = target.body.low
        }
    };
}

[[nodiscard]] studio_session::StudioTerrainViewportRuntimeSnapshot
ReferenceSnapshot(
    studio_session::StudioSession& session,
    const editor_session::ActiveBodyTarget& target,
    const world::WorldPosition& observer)
{
    studio_session::StudioTerrainViewportRuntimeSnapshot snapshot{};
    snapshot.worldGeneration = session.World().Generation();
    snapshot.universeGeneration = target.universeGeneration;
    snapshot.semanticBody = target.semanticObject;
    snapshot.body = target.body;
    snapshot.planet = ReferencePlanet(session, target);
    snapshot.observer = observer;
    return snapshot;
}
} // namespace

bool StudioRenderViewSet::HasTerrainNavigation(
    const std::string_view id) const
{
    if (session_ == nullptr)
    {
        return false;
    }

    const auto terrain =
        session_->TerrainRuntime().
            Capture(id);

    return terrain.has_value() &&
        session_->TerrainRuntime().
            IsCurrent(*terrain);
}

bool StudioRenderViewSet::NavigateReference(
    const std::string_view id,
    const StudioTerrainNavigationInput& input)
{
    render_view::RenderView* const view = Find(id);
    if (view == nullptr || session_ == nullptr)
    {
        return false;
    }

    const auto* viewport = session_->Viewports().Find(id);
    if (viewport == nullptr ||
        !viewport->target.has_value() ||
        viewport->mode != studio_session::ViewportMode::Perspective)
    {
        return false;
    }

    const auto& target = *viewport->target;

    auto found = referenceNavigation_.find(id);
    if (found == referenceNavigation_.end() ||
        found->second.target != target.semanticObject ||
        found->second.universeGeneration != target.universeGeneration)
    {
        // Continue from what the viewport shows now so the camera does not
        // jump when the user first touches the controls.
        world::WorldPosition start{};
        const auto& current = view->Camera();
        if (current.frame == target.frame)
        {
            start.meters = current.localPositionMeters;
        }
        else if (const auto framed =
                     ComposeViewportCamera(
                         *viewport,
                         target.universeGeneration);
                 framed.has_value())
        {
            start.meters = framed->localPositionMeters;
        }

        found = referenceNavigation_.insert_or_assign(
            std::string(id),
            ReferenceNavigation{
                .target = target.semanticObject,
                .universeGeneration = target.universeGeneration,
                .observer = start
            }).first;
    }

    auto& entry = found->second;
    const ReferenceSphereSource source;

    auto snapshot = ReferenceSnapshot(*session_, target, entry.observer);
    const StudioTerrainNavigationUpdate update =
        AdvanceTerrainNavigation(
            entry.navigation,
            snapshot,
            source,
            input);

    if (update.moved)
    {
        entry.observer = update.observer;
        snapshot.observer = update.observer;
    }

    view->Camera() =
        ComposeTerrainViewportCamera(
            *viewport,
            snapshot,
            &update);

    return
        update.moved ||
        input.mouseDeltaX != 0.0 ||
        input.mouseDeltaY != 0.0;
}

namespace
{
// The runtime's terrain source with the drawn ground as a floor: a query close
// to where the renderer read the ground back returns at least that elevation.
// Navigation (ground clearance, altitude above terrain) then agrees with what is
// on screen even when the CPU terrain is lower there.
class RenderedGroundFloorSource final : public terrain::TerrainSource
{
public:
    RenderedGroundFloorSource(
        const terrain::TerrainSource& base,
        const math::Double3& unitDirection,
        const f64 elevationMeters,
        const f64 planetRadiusMeters)
        : base_(base),
          direction_(unitDirection),
          elevation_(elevationMeters),
          // About 150 m on the surface.
          minimumCosine_(std::cos(150.0 / std::max(planetRadiusMeters, 1.0)))
    {
    }

    [[nodiscard]] terrain::TerrainSample Sample(
        const terrain::TerrainQuery& query) const noexcept override
    {
        terrain::TerrainSample sample = base_.Sample(query);
        if (math::Dot(math::Normalize(query.unitDirection), direction_) >=
                minimumCosine_ &&
            std::isfinite(elevation_) &&
            elevation_ > sample.elevationMeters)
        {
            sample.elevationMeters = elevation_;
        }
        return sample;
    }

    [[nodiscard]] u64 Revision() const noexcept override
    {
        return base_.Revision();
    }

    [[nodiscard]] terrain::TerrainGenerationRevisions GenerationRevisions()
        const noexcept override
    {
        return base_.GenerationRevisions();
    }

private:
    const terrain::TerrainSource& base_;
    math::Double3 direction_;
    f64 elevation_;
    f64 minimumCosine_;
};
} // namespace

void StudioRenderViewSet::SetRenderedGround(
    const std::string_view id,
    const math::Double3& unitDirection,
    const f64 elevationMeters)
{
    renderedGround_.insert_or_assign(
        std::string(id),
        RenderedGround{math::Normalize(unitDirection), elevationMeters});
}

bool StudioRenderViewSet::NavigateTerrain(
    const std::string_view id,
    const StudioTerrainNavigationInput& input)
{
    if (session_ == nullptr)
    {
        return false;
    }

    auto terrain =
        session_->TerrainRuntime().
            Capture(id);

    if (!terrain.has_value() ||
        !session_->TerrainRuntime().
            IsCurrent(*terrain))
    {
        return NavigateReference(id, input);
    }

    auto& state =
        RequireNavigationState(id);

    const auto& baseSource =
        session_->TerrainRuntime().
            TerrainSource(*terrain);

    const auto drawnGround = renderedGround_.find(id);
    std::optional<RenderedGroundFloorSource> flooredSource;
    if (drawnGround != renderedGround_.end())
    {
        flooredSource.emplace(
            baseSource,
            drawnGround->second.unitDirection,
            drawnGround->second.elevationMeters,
            terrain->planet.radiusMeters);
    }
    const terrain::TerrainSource& source =
        flooredSource.has_value()
            ? static_cast<const terrain::TerrainSource&>(*flooredSource)
            : baseSource;

    const StudioTerrainNavigationUpdate update =
        AdvanceTerrainNavigation(
            state,
            *terrain,
            source,
            input);

    if (update.moved)
    {
        static_cast<void>(
            session_->TerrainRuntime().
                SetObserver(
                    id,
                    update.observer));
    }

    return
        update.moved ||
        input.mouseDeltaX != 0.0 ||
        input.mouseDeltaY != 0.0;
}

bool StudioRenderViewSet::FocusTerrainBody(
    const std::string_view id)
{
    if (session_ == nullptr)
    {
        return false;
    }

    referenceNavigation_.erase(id);

    auto terrain =
        session_->TerrainRuntime().
            Capture(id);

    if (!terrain.has_value() ||
        !session_->TerrainRuntime().
            IsCurrent(*terrain))
    {
        return false;
    }

    auto& state =
        RequireNavigationState(id);

    const auto& source =
        session_->TerrainRuntime().
            TerrainSource(*terrain);

    const StudioTerrainNavigationUpdate update =
        studio_ui::FocusTerrainBody(
            state,
            *terrain,
            source);

    static_cast<void>(
        session_->TerrainRuntime().
            SetObserver(
                id,
                update.observer));

    return true;
}

bool StudioRenderViewSet::FocusTerrainSurfacePoint(
    const std::string_view id,
    const f32 u,
    const f32 v)
{
    if (session_ == nullptr)
    {
        return false;
    }

    auto terrain =
        session_->TerrainRuntime().
            Capture(id);

    if (!terrain.has_value() ||
        !session_->TerrainRuntime().
            IsCurrent(*terrain))
    {
        return false;
    }

    const auto selection =
        PickTerrainSurface(
            id,
            u,
            v,
            terrain->
                physicalPageLevel);

    if (!selection.has_value())
    {
        return false;
    }

    auto& state =
        RequireNavigationState(id);

    const auto& source =
        session_->TerrainRuntime().
            TerrainSource(*terrain);

    const StudioTerrainNavigationUpdate update =
        FocusTerrainSurface(
            state,
            *terrain,
            source,
            selection->
                surface.
                unitDirection);

    static_cast<void>(
        session_->TerrainRuntime().
            SetObserver(
                id,
                update.observer));

    return true;
}

void StudioRenderViewSet::SetFlatMapLayer(
    const std::string_view id,
    const FlatMapLayer layer)
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range("Studio render-view ID is not registered.");
    }
    flatMapLayers_.insert_or_assign(std::string(id), layer);
}

FlatMapLayer StudioRenderViewSet::FlatMapLayerOf(
    const std::string_view id) const
{
    const auto found = flatMapLayers_.find(id);
    return found == flatMapLayers_.end()
        ? FlatMapLayer::Elevation
        : found->second;
}

void StudioRenderViewSet::SetFlatMapStatus(
    const std::string_view id,
    const std::optional<StudioFlatMapStatus>& status)
{
    if (!status.has_value())
    {
        const auto found = flatMapStatuses_.find(id);
        if (found != flatMapStatuses_.end())
        {
            flatMapStatuses_.erase(found);
        }
        return;
    }
    flatMapStatuses_.insert_or_assign(std::string(id), *status);
}

std::optional<StudioFlatMapStatus> StudioRenderViewSet::FlatMapStatusOf(
    const std::string_view id) const
{
    const auto found = flatMapStatuses_.find(id);
    if (found == flatMapStatuses_.end())
    {
        return std::nullopt;
    }
    return found->second;
}

std::optional<math::Double3> StudioRenderViewSet::PickPlanetDirection(
    const std::string_view id,
    const f32 u,
    const f32 v) const
{
    const auto* view = Find(id);
    if (view == nullptr || session_ == nullptr)
    {
        return std::nullopt;
    }
    const auto* target = session_->Viewports().Find(id);
    if (target == nullptr)
    {
        return std::nullopt;
    }

    if (target->mode == studio_session::ViewportMode::FlatMap)
    {
        const auto mapUv =
            FlatMapUvFromViewUv(view->Width(), view->Height(), u, v);
        if (!mapUv.has_value())
        {
            return std::nullopt;
        }
        const auto latLon = FlatMapLatLonFromUv(*mapUv);
        return FlatMapDirectionFromLatLon(
            latLon.latitudeDegrees, latLon.longitudeDegrees);
    }

    if (target->mode == studio_session::ViewportMode::BodyMap)
    {
        const auto terrain = session_->TerrainRuntime().Capture(id);
        if (!terrain.has_value())
        {
            return std::nullopt;
        }
        return GlobePickDirection(
            view->Camera(),
            terrain->planet.radiusMeters,
            view->Width(),
            view->Height(),
            u,
            v);
    }

    return std::nullopt;
}

void StudioRenderViewSet::SetViewportMode(
    const std::string_view id,
    const studio_session::ViewportMode mode)
{
    if (session_ == nullptr || Find(id) == nullptr)
    {
        throw std::out_of_range("Studio render-view ID is not registered.");
    }
    session_->Viewports().SetMode(id, mode);
}

std::optional<std::pair<u32, u32>> StudioRenderViewSet::ViewSize(
    const std::string_view id) const
{
    const auto* view = Find(id);
    if (view == nullptr)
    {
        return std::nullopt;
    }
    return std::pair<u32, u32>{view->Width(), view->Height()};
}

bool StudioRenderViewSet::FocusTerrainDirection(
    const std::string_view id,
    const math::Double3& unitDirection)
{
    if (session_ == nullptr)
    {
        return false;
    }

    const auto terrain = session_->TerrainRuntime().Capture(id);
    if (!terrain.has_value() ||
        !session_->TerrainRuntime().IsCurrent(*terrain))
    {
        return false;
    }

    auto& state = RequireNavigationState(id);
    const auto& source = session_->TerrainRuntime().TerrainSource(*terrain);

    const StudioTerrainNavigationUpdate update = FocusTerrainSurface(
        state,
        *terrain,
        source,
        math::Normalize(unitDirection));

    static_cast<void>(
        session_->TerrainRuntime().SetObserver(id, update.observer));
    return true;
}

f64 StudioRenderViewSet::ViewFovRadians(const std::string_view id) const
{
    const render_view::RenderView* view = Find(id);
    if (view == nullptr)
    {
        throw std::out_of_range("Studio render-view ID is not registered.");
    }
    return view->Camera().verticalFovRadians;
}

void StudioRenderViewSet::SetCaptureTile(
    const std::string_view id,
    const std::optional<StudioCaptureTile>& tile)
{
    if (!tile.has_value())
    {
        captureTiles_.erase(std::string(id));
        return;
    }
    if (Find(id) == nullptr)
    {
        throw std::out_of_range("Studio render-view ID is not registered.");
    }
    captureTiles_.insert_or_assign(std::string(id), *tile);
}

void StudioRenderViewSet::SetZoom(const std::string_view id, const f64 zoom)
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range("Studio render-view ID is not registered.");
    }
    if (!std::isfinite(zoom))
    {
        throw std::invalid_argument("Zoom must be a finite number.");
    }
    zoom_.insert_or_assign(
        std::string(id), std::clamp(zoom, kMinZoom, kMaxZoom));
}

f64 StudioRenderViewSet::Zoom(const std::string_view id) const
{
    const auto found = zoom_.find(id);
    return found == zoom_.end() ? 1.0 : found->second;
}

std::optional<StudioViewPose> StudioRenderViewSet::ViewPose(
    const std::string_view id) const
{
    if (session_ == nullptr)
    {
        return std::nullopt;
    }

    const auto* viewport = session_->Viewports().Find(id);
    if (viewport == nullptr || !viewport->target.has_value())
    {
        return std::nullopt;
    }

    StudioViewPose pose{};
    pose.targetObject = viewport->target->semanticObject.ToString();
    pose.zoom = Zoom(id);

    const auto terrain = session_->TerrainRuntime().Capture(id);
    if (terrain.has_value() &&
        session_->TerrainRuntime().IsCurrent(*terrain))
    {
        const auto& state = RequireNavigationState(id);
        pose.terrain = true;
        pose.observerMeters = terrain->observer.meters;
        pose.surfaceFrame = state.surfaceFrame;
        pose.yawRadians = state.freeCamera.YawRadians();
        pose.pitchRadians = state.freeCamera.PitchRadians();
        return pose;
    }

    const auto reference = referenceNavigation_.find(id);
    if (reference == referenceNavigation_.end())
    {
        pose.hasCamera = false;
        return pose;
    }

    pose.observerMeters = reference->second.observer.meters;
    pose.surfaceFrame = reference->second.navigation.surfaceFrame;
    pose.yawRadians = reference->second.navigation.freeCamera.YawRadians();
    pose.pitchRadians = reference->second.navigation.freeCamera.PitchRadians();
    return pose;
}

bool StudioRenderViewSet::RestoreViewPose(
    const std::string_view id,
    const StudioViewPose& pose)
{
    if (session_ == nullptr)
    {
        return false;
    }

    const auto* viewport = session_->Viewports().Find(id);
    if (viewport == nullptr || !viewport->target.has_value() ||
        viewport->target->semanticObject.ToString() != pose.targetObject)
    {
        return false;
    }

    SetZoom(id, pose.zoom);

    if (!pose.hasCamera)
    {
        return true;
    }

    camera::FreeCameraConfig cameraConfig{};
    cameraConfig.initialYawRadians = pose.yawRadians;
    cameraConfig.initialPitchRadians = pose.pitchRadians;

    const auto terrain = session_->TerrainRuntime().Capture(id);
    if (terrain.has_value() &&
        session_->TerrainRuntime().IsCurrent(*terrain))
    {
        auto& state = RequireNavigationState(id);
        SynchronizeTerrainNavigation(state, *terrain);
        state.surfaceFrame = pose.surfaceFrame;
        state.freeCamera = camera::FreeCamera(cameraConfig);
        return session_->TerrainRuntime().SetObserver(
            id,
            world::WorldPosition{.meters = pose.observerMeters});
    }

    // Reference-sphere body: make sure its navigation entry exists, then
    // overwrite it.
    static_cast<void>(NavigateReference(id, {}));
    const auto reference = referenceNavigation_.find(id);
    if (reference == referenceNavigation_.end())
    {
        return false;
    }

    reference->second.observer.meters = pose.observerMeters;
    reference->second.navigation.surfaceFrame = pose.surfaceFrame;
    reference->second.navigation.freeCamera =
        camera::FreeCamera(cameraConfig);
    return true;
}

std::optional<StudioSurfacePick>
StudioRenderViewSet::PickTerrainSurface(
    const std::string_view id,
    const f32 u,
    const f32 v,
    const std::optional<u8> physicalTileLevel)
{
    if (session_ == nullptr)
    {
        return std::nullopt;
    }

    auto* view =
        Find(id);

    const auto* target =
        session_->Viewports().
            Find(id);

    auto runtime =
        session_->TerrainRuntime().
            Capture(id);

    if (view == nullptr ||
        target == nullptr ||
        !runtime.has_value() ||
        !session_->TerrainRuntime().
            IsCurrent(*runtime))
    {
        return std::nullopt;
    }

    const auto& source =
        session_->TerrainRuntime().
            TerrainSource(*runtime);

    auto pick =
        PickStudioTerrainSurface(
            *target,
            view->Camera(),
            view->Width(),
            view->Height(),
            u,
            v,
            *runtime,
            source,
            physicalTileLevel);

    if (pick.has_value())
    {
        terrainSurfacePicks_.
            insert_or_assign(
                std::string(id),
                *pick);
    }

    return pick;
}

void StudioRenderViewSet::SetTerrainLayers(
    const std::string_view id,
    StudioTerrainLayerOptions options)
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }
    if (!std::isfinite(options.lodBiasStops))
    {
        options.lodBiasStops = 0.0F;
    }
    options.lodBiasStops = std::clamp(options.lodBiasStops, -4.0F, 4.0F);
    if (!std::isfinite(options.clipmapPixelsPerVertex))
    {
        options.clipmapPixelsPerVertex = 3.0F;
    }
    options.clipmapPixelsPerVertex =
        std::clamp(options.clipmapPixelsPerVertex, 0.25F, 32.0F);
    if (!std::isfinite(options.clipmapFadeSeconds))
    {
        options.clipmapFadeSeconds = 0.4F;
    }
    options.clipmapFadeSeconds =
        std::clamp(options.clipmapFadeSeconds, 0.0F, 5.0F);
    if (!std::isfinite(options.clipmapBandScale))
    {
        options.clipmapBandScale = 1.0F;
    }
    options.clipmapBandScale = std::clamp(options.clipmapBandScale, 0.1F, 10.0F);
    for (f32& edge : options.clipmapBandEdgesMeters)
    {
        if (!std::isfinite(edge) || edge < 0.0F)
        {
            edge = 0.0F;
        }
    }
    terrainLayers_.insert_or_assign(std::string(id), options);
}

StudioTerrainLayerOptions StudioRenderViewSet::TerrainLayers(
    const std::string_view id) const
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }
    const auto found = terrainLayers_.find(id);
    return found == terrainLayers_.end()
        ? StudioTerrainLayerOptions{}
        : found->second;
}

void StudioRenderViewSet::SetCloudReport(
    const std::string_view id,
    const std::optional<StudioCloudReport>& report)
{
    if (report.has_value())
        cloudReports_.insert_or_assign(std::string(id), *report);
    else
        cloudReports_.erase(std::string(id));
}

void StudioRenderViewSet::SetClipmapPlanStats(
    const std::string_view id,
    const StudioClipmapPlanStats& stats)
{
    clipmapPlanStats_.insert_or_assign(std::string(id), stats);
}

void StudioRenderViewSet::SetOrbitalPatchStats(
    const std::string_view id,
    const u32 patchesPending,
    const u32 patchesResident)
{
    orbitalPatchStats_.insert_or_assign(
        std::string(id),
        OrbitalPatchStats{patchesPending, patchesResident});
}

void StudioRenderViewSet::SetTextDiagnosticsHud(
    const std::string_view id,
    const bool enabled)
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }
    textDiagnosticsHud_.insert_or_assign(std::string(id), enabled);
}

bool StudioRenderViewSet::TextDiagnosticsHud(const std::string_view id) const
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }
    const auto found = textDiagnosticsHud_.find(id);
    return found != textDiagnosticsHud_.end() && found->second;
}

StudioViewportTextReport StudioRenderViewSet::TextDiagnostics(
    const std::string_view id,
    const std::optional<std::pair<f32, f32>> cursorUv)
{
    const auto* view = Find(id);
    if (view == nullptr || session_ == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    StudioViewportTextReport report;
    report.viewId = std::string(id);
    report.layers = TerrainLayers(id);
    report.width = view->Width();
    report.height = view->Height();

    const auto* target = session_->Viewports().Find(id);
    if (target != nullptr)
    {
        switch (target->mode)
        {
        case studio_session::ViewportMode::Perspective:
            report.viewMode = "perspective";
            break;
        case studio_session::ViewportMode::BodyMap:
            report.viewMode = "body_map";
            break;
        case studio_session::ViewportMode::Debug:
            report.viewMode = "debug";
            break;
        case studio_session::ViewportMode::System:
            report.viewMode = "system";
            break;
        case studio_session::ViewportMode::FlatMap:
            report.viewMode = "flat_map";
            break;
        }
    }

    const auto& camera = view->Camera();
    constexpr f64 kRadiansToDegrees = 180.0 / 3.14159265358979323846;
    report.cameraPosition = camera.localPositionMeters;
    report.forward = {camera.forward.x, camera.forward.y, camera.forward.z};
    report.up = {camera.up.x, camera.up.y, camera.up.z};
    report.verticalFovDegrees = camera.verticalFovRadians * kRadiansToDegrees;
    report.nearPlaneMeters = camera.nearPlaneMeters;
    report.farPlaneMeters = camera.farPlaneMeters;
    report.distanceFromCoreMeters = math::Length(camera.localPositionMeters);

    const auto runtime = session_->TerrainRuntime().Capture(id);
    const bool terrainCurrent =
        runtime.has_value() && session_->TerrainRuntime().IsCurrent(*runtime);

    report.navigation = terrainCurrent
        ? "terrain"
        : (target != nullptr && target->target.has_value()
            ? "reference_sphere"
            : "none");

    {
        StudioCpuTerrainReport cpu;
        const auto pool = session_->TerrainPhysicalPages().JobTelemetry();
        cpu.poolWorkers = pool.workers;
        cpu.poolRunningJobs = pool.running;
        cpu.poolQueuedJobs = pool.queued;
        cpu.poolOutstandingJobs = pool.outstanding;

        if (const auto patches = orbitalPatchStats_.find(id);
            patches != orbitalPatchStats_.end())
        {
            cpu.globePatchesPending = patches->second.pending;
            cpu.globePatchesResident = patches->second.resident;
        }

        if (terrainCurrent)
        {
            if (const auto status =
                    session_->TerrainPhysicalPages().BodyStatus(
                        runtime->planet.id);
                status.has_value())
            {
                cpu.hasPages = true;
                cpu.pageState =
                    studio_session::TerrainRebuildStateName(status->state);
                cpu.pages = status->pages;
                cpu.dirtyPages = status->dirtyPages;
                cpu.queuedPages = status->queuedPages;
                cpu.buildingPages = status->buildingPages;
                cpu.uploadingPages = status->uploadingPages;
                cpu.readyPages = status->readyPages;
                cpu.failedPages = status->failedPages;
                cpu.stalePages = status->stalePages;
                cpu.completedProducts = status->completedProducts;
                cpu.totalProducts = status->totalProducts;
            }
        }
        report.cpuTerrain = cpu;
    }

    if (const auto plan = clipmapPlanStats_.find(id);
        plan != clipmapPlanStats_.end() && plan->second.valid)
    {
        report.clipmapPlan = plan->second;
    }
    if (const auto clouds = cloudReports_.find(id); clouds != cloudReports_.end())
    {
        report.clouds = clouds->second;
    }

    if (!terrainCurrent)
    {
        return report;
    }

    const auto& source = session_->TerrainRuntime().TerrainSource(*runtime);
    const f64 radius = runtime->planet.radiusMeters;

    report.hasTerrain = true;
    report.planetRadiusMeters = radius;
    report.heightAboveDatumMeters = report.distanceFromCoreMeters - radius;
    report.physicalPageLevel = runtime->physicalPageLevel;
    report.adaptiveCoverageTier = runtime->adaptiveCoverageTier;
    report.terrainSourceRevision = runtime->terrainSourceRevision;
    report.worldGeneration = runtime->worldGeneration;
    report.runtimeGeneration = runtime->runtimeGeneration;

    if (report.distanceFromCoreMeters > 0.0)
    {
        const math::Double3 radial =
            camera.localPositionMeters / report.distanceFromCoreMeters;
        const math::Double3 pole{0.0, 1.0, 0.0};
        math::Double3 north = pole - radial * math::Dot(pole, radial);
        if (math::LengthSquared(north) < 1.0e-18)
        {
            north = {1.0, 0.0, 0.0};
        }
        north = math::Normalize(north);
        // Toward increasing longitude, matching the terrain point report.
        const math::Double3 east = math::Normalize(math::Cross(radial, pole));
        const math::Double3 forward = report.forward;
        report.cameraPitchDegrees =
            std::asin(std::clamp(math::Dot(forward, radial), -1.0, 1.0)) *
            kRadiansToDegrees;
        f64 heading =
            std::atan2(math::Dot(forward, east), math::Dot(forward, north)) *
            kRadiansToDegrees;
        if (heading < 0.0)
        {
            heading += 360.0;
        }
        report.cameraHeadingDegrees = heading;

        const f64 altitudeGuess =
            std::max(std::abs(report.heightAboveDatumMeters), 1.0);
        report.nadir = SampleStudioTerrainPoint(
            source,
            runtime->planet.id,
            radius,
            radial,
            StudioPixelFootprintMeters(
                camera.verticalFovRadians,
                report.height,
                altitudeGuess));
        report.heightAboveTerrainMeters =
            report.distanceFromCoreMeters -
            report.nadir->groundRadiusFromCoreMeters;
        if (report.nadir->underwater)
        {
            report.heightAboveWaterSurfaceMeters =
                report.distanceFromCoreMeters -
                report.nadir->renderedSurfaceRadiusFromCoreMeters;
        }
    }

    if (cursorUv.has_value())
    {
        const auto pick = PickTerrainSurface(id, cursorUv->first, cursorUv->second);
        if (pick.has_value())
        {
            StudioCursorPickReport cursor;
            cursor.point = SampleStudioTerrainPoint(
                source,
                runtime->planet.id,
                radius,
                pick->surface.unitDirection,
                StudioPixelFootprintMeters(
                    camera.verticalFovRadians,
                    report.height,
                    std::max(pick->hitDistanceMeters, 1.0)));
            cursor.hitDistanceMeters = pick->hitDistanceMeters;
            cursor.pickPhysicalElevationMeters = pick->physicalElevationMeters;
            cursor.pickRenderedElevationMeters = pick->renderedElevationMeters;
            cursor.physicalLod = pick->physicalLod;
            if (pick->physicalPage.has_value())
            {
                const auto& tile = pick->physicalPage->tile;
                cursor.physicalPage = std::format(
                    "{}/{}/{}/{}",
                    static_cast<u32>(tile.face),
                    static_cast<u32>(tile.level),
                    tile.x,
                    tile.y);
            }
            report.cursor = std::move(cursor);
        }
    }

    return report;
}

std::optional<StudioSurfacePick>
StudioRenderViewSet::LastTerrainSurfacePick(
    const std::string_view id) const
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    const auto found =
        terrainSurfacePicks_.find(id);

    return found ==
            terrainSurfacePicks_.end()
        ? std::nullopt
        : std::optional(found->second);
}

void StudioRenderViewSet::SetTerrainAuthoringOverlay(
    const std::string_view id,
    StudioTerrainAuthoringOverlay overlay)
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    if (!overlay.body.IsValid() ||
        overlay.controlUnitDirections.empty() ||
        !std::isfinite(
            overlay.influenceRadiusMeters) ||
        overlay.influenceRadiusMeters < 0.0)
    {
        throw std::invalid_argument(
            "Studio terrain authoring overlay is invalid.");
    }

    for (auto& direction :
         overlay.controlUnitDirections)
    {
        const f64 length =
            math::Length(direction);

        if (!std::isfinite(length) ||
            length <= 1.0e-12)
        {
            throw std::invalid_argument(
                "Studio terrain authoring overlay contains an invalid direction.");
        }

        direction =
            direction / length;
    }

    terrainAuthoringOverlays_.
        insert_or_assign(
            std::string(id),
            std::move(overlay));
}

void StudioRenderViewSet::ClearTerrainAuthoringOverlay(
    const std::string_view id) noexcept
{
    terrainAuthoringOverlays_.erase(id);
}

std::optional<StudioTerrainAuthoringOverlay>
StudioRenderViewSet::TerrainAuthoringOverlay(
    const std::string_view id) const
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    const auto found =
        terrainAuthoringOverlays_.find(id);

    return found ==
            terrainAuthoringOverlays_.end()
        ? std::nullopt
        : std::optional(found->second);
}

void StudioRenderViewSet::SetTerrainDiagnosticOverlays(
    const std::string_view id,
    const StudioTerrainDiagnosticOverlayOptions options)
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    terrainDiagnosticOverlays_.insert_or_assign(
        std::string(id),
        options);
}

StudioTerrainDiagnosticOverlayOptions
StudioRenderViewSet::TerrainDiagnosticOverlays(
    const std::string_view id) const
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    const auto found =
        terrainDiagnosticOverlays_.find(id);

    return found ==
            terrainDiagnosticOverlays_.end()
        ? StudioTerrainDiagnosticOverlayOptions{}
        : found->second;
}

bool StudioRenderViewSet::ResetTerrainView(
    const std::string_view id)
{
    if (session_ == nullptr)
    {
        return false;
    }

    referenceNavigation_.erase(id);

    auto terrain =
        session_->TerrainRuntime().
            Capture(id);

    if (!terrain.has_value() ||
        !session_->TerrainRuntime().
            IsCurrent(*terrain))
    {
        return false;
    }

    auto& state =
        RequireNavigationState(id);

    const auto& source =
        session_->TerrainRuntime().
            TerrainSource(*terrain);

    static_cast<void>(
        ResetTerrainNavigationOrientation(
            state,
            *terrain,
            source));

    return true;
}

void StudioRenderViewSet::SetDebugField(
    const std::string_view id,
    const terrain_debug::TerrainDebugField field)
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    static_cast<void>(
        terrain_debug::Descriptor(field));

    debugFields_[std::string(id)] = field;
}

terrain_debug::TerrainDebugField
StudioRenderViewSet::DebugField(
    const std::string_view id) const
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    const auto found = debugFields_.find(id);
    if (found == debugFields_.end())
    {
        return terrain_debug::TerrainDebugField::Uplift;
    }

    return found->second;
}

void StudioRenderViewSet::SetSurfaceDebugMode(
    const std::string_view id,
    const lighting::SurfaceDebugMode mode)
{
    auto* view = Find(id);

    if (view == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    view->SetSurfaceDebugMode(mode);
}

lighting::SurfaceDebugMode StudioRenderViewSet::SurfaceDebugMode(
    const std::string_view id) const
{
    const auto* view = Find(id);

    if (view == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    return view->SurfaceDebugMode();
}

void StudioRenderViewSet::SetDebugPhysicalPageLevel(
    const std::string_view id,
    const u8 level)
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    if (level > 30U)
    {
        throw std::invalid_argument(
            "Physical terrain page tile level must be in [0,30].");
    }

    debugPhysicalPageLevels_[std::string(id)] = level;
    debugPhysicalPages_.erase(std::string(id));
    liveDebugPages_.erase(std::string(id));
}

u8 StudioRenderViewSet::DebugPhysicalPageLevel(
    const std::string_view id) const
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    const auto found =
        debugPhysicalPageLevels_.find(id);

    return found == debugPhysicalPageLevels_.end()
        ? static_cast<u8>(8)
        : found->second;
}

bool StudioRenderViewSet::SelectDebugPhysicalPage(
    const std::string_view id,
    const f32 u,
    const f32 v)
{
    if (Find(id) == nullptr ||
        session_ == nullptr)
    {
        return false;
    }

    const auto pick =
        PickTerrainSurface(
            id,
            u,
            v,
            DebugPhysicalPageLevel(id));

    if (!pick.has_value() ||
        !pick->physicalPage.has_value())
    {
        return false;
    }

    debugPhysicalPages_.insert_or_assign(
        std::string(id),
        StudioPhysicalPageSelection{
            .address =
                *pick->physicalPage,
            .surfaceDirection =
                pick->surface.
                    unitDirection
        });
    liveDebugPages_.erase(std::string(id));
    return true;
}

std::optional<StudioPhysicalPageSelection>
StudioRenderViewSet::DebugPhysicalPage(
    const std::string_view id) const
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    const auto found =
        debugPhysicalPages_.find(id);

    return found == debugPhysicalPages_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::shared_ptr<const terrain_debug::TerrainDebugPageData>
StudioRenderViewSet::LiveDebugPage(
    const std::string_view id) const
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    const auto found =
        liveDebugPages_.find(id);

    return found == liveDebugPages_.end()
        ? nullptr
        : found->second;
}

u32 StudioRenderViewSet::Refresh(
    const studio_session::StudioRuntimeSnapshot& snapshot)
{
    RequireCurrentSnapshot(snapshot);

    u32 targetedViews = 0;

    for (auto& [id, view] : views_)
    {
        const auto* target =
            session_->Viewports().Find(id);

        if (target == nullptr)
        {
            throw std::logic_error(
                "Studio RenderView lost its logical viewport target slot.");
        }

        std::optional<
            render_view::CameraState>
            camera;

        if (target->mode ==
                studio_session::ViewportMode::Perspective)
        {
            const auto terrain =
                session_->TerrainRuntime().
                    Capture(id);

            if (terrain.has_value())
            {
                if (!session_->
                        TerrainRuntime().
                        IsCurrent(*terrain))
                {
                    throw std::logic_error(
                        "Studio RenderView received a stale terrain runtime snapshot.");
                }

                auto& navigationState =
                    RequireNavigationState(id);

                const auto& source =
                    session_->TerrainRuntime().
                        TerrainSource(*terrain);

                const StudioTerrainNavigationUpdate navigation =
                    CurrentTerrainNavigation(
                        navigationState,
                        *terrain,
                        source);

                camera =
                    ComposeTerrainViewportCamera(
                        *target,
                        *terrain,
                        &navigation);
                referenceNavigation_.erase(id);
            }
            else
            {
                RequireNavigationState(id).
                    initialized =
                    false;
            }
        }

        if (!camera.has_value() &&
            target->mode ==
                studio_session::ViewportMode::Perspective)
        {
            const auto reference = referenceNavigation_.find(id);
            if (reference != referenceNavigation_.end())
            {
                if (target->target.has_value() &&
                    target->target->semanticObject ==
                        reference->second.target &&
                    target->target->universeGeneration ==
                        reference->second.universeGeneration)
                {
                    const ReferenceSphereSource source;
                    const auto referenceRuntime = ReferenceSnapshot(
                        *session_,
                        *target->target,
                        reference->second.observer);
                    const auto navigation =
                        CurrentTerrainNavigation(
                            reference->second.navigation,
                            referenceRuntime,
                            source);
                    camera = ComposeTerrainViewportCamera(
                        *target,
                        referenceRuntime,
                        &navigation);
                }
                else
                {
                    referenceNavigation_.erase(reference);
                }
            }
        }

        if (!camera.has_value())
        {
            camera =
                ComposeViewportCamera(
                    *target,
                    snapshot.
                        universeGeneration);
        }

        if (!camera.has_value())
        {
            view->Camera() = {};
            debugPhysicalPages_.erase(id);
            terrainSurfacePicks_.erase(id);
            terrainAuthoringOverlays_.erase(id);
            liveDebugPages_.erase(id);
            continue;
        }

        if (const auto zoom = zoom_.find(id);
            zoom != zoom_.end() && zoom->second != 1.0)
        {
            camera->verticalFovRadians = static_cast<f32>(
                2.0 *
                std::atan(
                    std::tan(0.5 * camera->verticalFovRadians) /
                    zoom->second));
        }

        if (const auto tile = captureTiles_.find(id);
            tile != captureTiles_.end())
        {
            const math::Float3 forward = math::Normalize(camera->forward);
            const math::Float3 up = math::Normalize(camera->up);
            // The view frame the renderers use is mirrored relative to the world
            // vectors in the camera state, so screen-right is forward x up.
            const math::Float3 right = math::Normalize(math::Cross(forward, up));

            const f64 sinYaw = std::sin(tile->second.yawRadians);
            const f64 cosYaw = std::cos(tile->second.yawRadians);
            const f64 sinPitch = std::sin(tile->second.pitchRadians);
            const f64 cosPitch = std::cos(tile->second.pitchRadians);

            // Camera-space basis (x right, y up, z forward) of the turned view.
            const math::Double3 newForward{
                cosPitch * sinYaw, sinPitch, cosPitch * cosYaw};
            const math::Double3 newRight{cosYaw, 0.0, -sinYaw};
            const math::Double3 newUp = math::Cross(newForward, newRight);

            const auto toWorld = [&](const math::Double3& v)
            {
                return math::Normalize(math::Float3{
                    static_cast<f32>(
                        v.x * right.x + v.y * up.x + v.z * forward.x),
                    static_cast<f32>(
                        v.x * right.y + v.y * up.y + v.z * forward.y),
                    static_cast<f32>(
                        v.x * right.z + v.y * up.z + v.z * forward.z)});
            };

            camera->forward = toWorld(newForward);
            camera->up = toWorld(newUp);
            camera->verticalFovRadians =
                static_cast<f32>(tile->second.verticalFovRadians);
        }

        ApplyViewportCamera(
            *view,
            *camera);

        const auto selected =
            debugPhysicalPages_.find(id);

        if (target->target.has_value() &&
            selected != debugPhysicalPages_.end())
        {
            const world::PlanetId expectedPlanet{
                .high = target->target->body.high,
                .low = target->target->body.low
            };

            if (selected->second.address.planet !=
                expectedPlanet)
            {
                debugPhysicalPages_.erase(selected);
                terrainSurfacePicks_.erase(id);
                terrainAuthoringOverlays_.erase(id);
                liveDebugPages_.erase(id);
            }
        }

        if (target->mode ==
                studio_session::ViewportMode::Debug &&
            !debugPhysicalPages_.contains(id))
        {
            static_cast<void>(
                SelectDebugPhysicalPage(
                    id,
                    0.5F,
                    0.5F));
        }

        const auto selectedPage =
            debugPhysicalPages_.find(id);

        if (target->mode ==
                studio_session::ViewportMode::Debug &&
            selectedPage != debugPhysicalPages_.end())
        {
            auto live =
                session_->TerrainDebugPages().Find(
                    selectedPage->second.address);

            if (live != nullptr)
            {
                liveDebugPages_.insert_or_assign(
                    id,
                    std::move(live));
            }
            else
            {
                liveDebugPages_.erase(id);
            }
        }
        else
        {
            liveDebugPages_.erase(id);
        }

        ++targetedViews;
    }

    return targetedViews;
}

render_view::RenderView* StudioRenderViewSet::Find(
    const std::string_view id) noexcept
{
    const auto found = views_.find(id);
    return found == views_.end()
        ? nullptr
        : found->second.get();
}

const render_view::RenderView* StudioRenderViewSet::Find(
    const std::string_view id) const noexcept
{
    const auto found = views_.find(id);
    return found == views_.end()
        ? nullptr
        : found->second.get();
}

std::vector<StudioRenderViewInfo>
StudioRenderViewSet::Catalog() const
{
    std::vector<StudioRenderViewInfo> result;
    result.reserve(views_.size());

    for (const auto& [id, view] : views_)
    {
        const auto* target =
            session_ != nullptr
                ? session_->Viewports().Find(id)
                : nullptr;

        result.push_back({
            .id = id,
            .mode = target != nullptr
                ? target->mode
                : studio_session::ViewportMode::Perspective,
            .width = view->Width(),
            .height = view->Height(),
            .hasTarget =
                target != nullptr &&
                target->target.has_value(),
            .surfaceDebugMode =
                SurfaceDebugMode(id),
            .debugField =
                DebugField(id),
            .debugPhysicalPageLevel =
                DebugPhysicalPageLevel(id),
            .debugPhysicalPage =
                DebugPhysicalPage(id),
            .hasLiveDebugPage =
                LiveDebugPage(id) != nullptr,
            .diagnostics =
                TerrainDiagnosticOverlays(id),
            .layers =
                TerrainLayers(id),
            .flatMapLayer =
                FlatMapLayerOf(id)
        });
    }

    return result;
}

void StudioRenderViewSet::RequireCurrentSnapshot(
    const studio_session::StudioRuntimeSnapshot& snapshot) const
{
    if (session_ == nullptr)
    {
        throw std::logic_error(
            "Studio render-view set has no session binding.");
    }

    const auto& world = session_->World();

    if (snapshot.hasWorld != world.HasWorld() ||
        snapshot.worldGeneration != world.Generation() ||
        snapshot.universeGeneration != world.UniverseGeneration())
    {
        throw std::logic_error(
            "Studio RenderView refresh received a stale runtime snapshot.");
    }
}

StudioTerrainNavigationState&
StudioRenderViewSet::RequireNavigationState(
    const std::string_view id)
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    const auto found =
        navigationStates_.find(id);

    if (found == navigationStates_.end())
    {
        throw std::logic_error(
            "Studio render-view lost its navigation state.");
    }

    return found->second;
}

const StudioTerrainNavigationState&
StudioRenderViewSet::RequireNavigationState(
    const std::string_view id) const
{
    if (Find(id) == nullptr)
    {
        throw std::out_of_range(
            "Studio render-view ID is not registered.");
    }

    const auto found =
        navigationStates_.find(id);

    if (found == navigationStates_.end())
    {
        throw std::logic_error(
            "Studio render-view lost its navigation state.");
    }

    return found->second;
}
} // namespace orbit::studio_ui
