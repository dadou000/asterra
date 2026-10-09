#include <orbit/studio_ui/StudioViewportPanels.hpp>
#include <orbit/editor_model/CelestialAuthoringModel.hpp>

#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/studio_ui/StudioShellModel.hpp>
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/AtmospherePropertySolver.hpp>
#include <orbit/world_model/WorldSchemas.hpp>
#include <orbit/world_model/VisibilityProxyBinding.hpp>
#include <orbit/world_model/LocalLightBinding.hpp>
#include <orbit/world/PlanetTileNeighborhood.hpp>
#include <orbit/terrain_erosion/RiverNetwork.hpp>
#include <orbit/studio_session/StudioTerrainTectonicsProbe.hpp>
#include <orbit/terrain_bake/TerrainBakeService.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <orbit/studio_ui/StudioViewportPanels.hpp>

#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/studio_session/StudioTerrainAuthoringInvalidation.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

namespace orbit::studio_ui
{
namespace
{
constexpr commands::CommandId kViewportPerspectiveCommand{
    .high = 0x4f52424954564d4fULL,
    .low = 0x0000000000000001ULL
};

constexpr commands::CommandId kViewportBodyMapCommand{
    .high = 0x4f52424954564d4fULL,
    .low = 0x0000000000000002ULL
};

constexpr commands::CommandId kViewportDebugCommand{
    .high = 0x4f52424954564d4fULL,
    .low = 0x0000000000000003ULL
};

[[nodiscard]] const char* TerrainToolName(
    const StudioTerrainAuthoringTool tool) noexcept
{
    switch (tool)
    {
    case StudioTerrainAuthoringTool::Select: return "Select";
    case StudioTerrainAuthoringTool::Raise: return "Raise";
    case StudioTerrainAuthoringTool::Lower: return "Lower";
    case StudioTerrainAuthoringTool::Protection: return "Protect";
    case StudioTerrainAuthoringTool::Drainage: return "Drainage";
    case StudioTerrainAuthoringTool::DrainagePath: return "Drainage Path";
    case StudioTerrainAuthoringTool::Canyon: return "Canyon";
    case StudioTerrainAuthoringTool::Ridge: return "Ridge";
    case StudioTerrainAuthoringTool::Material: return "Geology";
    case StudioTerrainAuthoringTool::BiomePaint: return "Biome Paint";
    }
    return "Select";
}

[[nodiscard]] bool IsSplineTool(
    const StudioTerrainAuthoringTool tool) noexcept
{
    return
        tool == StudioTerrainAuthoringTool::Canyon ||
        tool == StudioTerrainAuthoringTool::Ridge ||
        tool == StudioTerrainAuthoringTool::DrainagePath;
}

[[nodiscard]] const char* CubeFaceName(
    const world::CubeFace face) noexcept
{
    switch (face)
    {
    case world::CubeFace::PositiveX:
        return "+X";
    case world::CubeFace::NegativeX:
        return "-X";
    case world::CubeFace::PositiveY:
        return "+Y";
    case world::CubeFace::NegativeY:
        return "-Y";
    case world::CubeFace::PositiveZ:
        return "+Z";
    case world::CubeFace::NegativeZ:
        return "-Z";
    }

    return "?";
}

[[nodiscard]] std::optional<scene::ObjectId>
SelectedBiomeObject(
    studio_session::StudioSession& session)
{
    if (!session.World().HasWorld() ||
        session.World().Selection().Ordered().size() != 1U)
    {
        return std::nullopt;
    }

    auto cursor =
        session.World().Selection().Ordered().front();

    for (u32 depth = 0U; depth < 32U; ++depth)
    {
        const auto record =
            session.World().Objects().Find(cursor);

        if (!record.has_value())
        {
            return std::nullopt;
        }

        if (record->type ==
            world_model::kBiomeAssetType)
        {
            return record->id;
        }

        if (!record->parent.has_value())
        {
            return std::nullopt;
        }

        cursor = *record->parent;
    }

    return std::nullopt;
}

[[nodiscard]] const char* BiomeOperationName(
    const terrain_biome::BiomeAuthoredWeightOperation operation) noexcept
{
    switch (operation)
    {
    case terrain_biome::BiomeAuthoredWeightOperation::Add:
        return "Add";
    case terrain_biome::BiomeAuthoredWeightOperation::Subtract:
        return "Subtract";
    case terrain_biome::BiomeAuthoredWeightOperation::Replace:
        return "Replace";
    case terrain_biome::BiomeAuthoredWeightOperation::Multiply:
        return "Multiply";
    case terrain_biome::BiomeAuthoredWeightOperation::Min:
        return "Min";
    case terrain_biome::BiomeAuthoredWeightOperation::Max:
        return "Max";
    }

    return "Replace";
}
} // namespace

struct StudioViewportPanels::ViewportModeCommandState
{
    StudioViewportPanels* owner{nullptr};
};

void StudioViewportPanels::UnregisterViewportModeCommands() noexcept
{
    try
    {
        if (session_ == nullptr ||
            !session_->World().HasWorld())
        {
            return;
        }

        auto& registry = session_->World().CommandRegistry();
        static_cast<void>(
            registry.Unregister(kViewportPerspectiveCommand));
        static_cast<void>(
            registry.Unregister(kViewportBodyMapCommand));
        static_cast<void>(
            registry.Unregister(kViewportDebugCommand));
    }
    catch (...)
    {
        // World teardown may already have replaced its command graph. There is
        // nothing left to unregister in that case.
    }
}

void StudioViewportPanels::EnsureViewportModeCommands()
{
    if (session_ == nullptr ||
        !session_->World().HasWorld())
    {
        return;
    }

    if (!viewportModeCommandState_)
    {
        viewportModeCommandState_ =
            std::make_shared<ViewportModeCommandState>();
    }
    viewportModeCommandState_->owner = this;

    const std::weak_ptr<ViewportModeCommandState> weakState{
        viewportModeCommandState_};
    auto& registry = session_->World().CommandRegistry();

    const auto registerMode =
        [&registry, weakState](
            const commands::CommandId command,
            const std::string_view name,
            const studio_session::ViewportMode mode)
        {
            if (registry.Find(command) != nullptr)
            {
                return;
            }

            registry.Register({
                .id = command,
                .name = std::string{name},
                .category = "Viewport",
                .description =
                    "Switch the focus-aware Studio viewport to " +
                    std::string{name} + ".",
                .presentationSurfaces = {
                    std::string{kStudioContextCommandSurface}
                },
                .automationVisible = true,
                .enablement =
                    [weakState, mode]() -> commands::CommandEnablement
                    {
                        const auto state = weakState.lock();
                        if (!state ||
                            state->owner == nullptr ||
                            state->owner->session_ == nullptr ||
                            !state->owner->session_->World().HasWorld())
                        {
                            return {
                                .enabled = false,
                                .reason = "No active Studio world."
                            };
                        }

                        const std::string_view id =
                            state->owner->expansion_.ControlledViewportId();
                        const auto* viewport =
                            state->owner->session_->Viewports().Find(id);
                        if (viewport == nullptr)
                        {
                            return {
                                .enabled = false,
                                .reason = "Controlled viewport is unavailable."
                            };
                        }

                        if (viewport->mode == mode)
                        {
                            return {
                                .enabled = false,
                                .reason = "Already active."
                            };
                        }

                        return {};
                    },
                .invoke =
                    [weakState, mode](
                        const commands::CommandArguments&)
                    {
                        const auto state = weakState.lock();
                        if (!state ||
                            state->owner == nullptr ||
                            state->owner->session_ == nullptr ||
                            !state->owner->session_->World().HasWorld())
                        {
                            throw std::logic_error(
                                "Viewport mode command lost its Studio session.");
                        }

                        const std::string_view id =
                            state->owner->expansion_.ControlledViewportId();
                        if (state->owner->session_->Viewports().Find(id) == nullptr)
                        {
                            throw std::logic_error(
                                "Controlled viewport is unavailable.");
                        }

                        state->owner->session_->Viewports().SetMode(
                            id,
                            mode);
                    }
            });
        };

    registerMode(
        kViewportPerspectiveCommand,
        "Perspective",
        studio_session::ViewportMode::Perspective);
    registerMode(
        kViewportBodyMapCommand,
        "Body Map",
        studio_session::ViewportMode::BodyMap);
    registerMode(
        kViewportDebugCommand,
        "Debug",
        studio_session::ViewportMode::Debug);

    // Row 2 owns the compact presentation; command objects stay registered
    // independently for automation and command search.
}

void StudioViewportPanels::InvokeViewportMode(
    const studio_session::ViewportMode mode)
{
    if (session_ == nullptr ||
        !session_->World().HasWorld())
    {
        throw std::logic_error(
            "Viewport mode selection requires an active Studio world.");
    }

    EnsureViewportModeCommands();

    switch (mode)
    {
    case studio_session::ViewportMode::Perspective:
        session_->World().CommandRegistry().Invoke(
            kViewportPerspectiveCommand);
        return;

    case studio_session::ViewportMode::BodyMap:
        session_->World().CommandRegistry().Invoke(
            kViewportBodyMapCommand);
        return;

    case studio_session::ViewportMode::Debug:
        session_->World().CommandRegistry().Invoke(
            kViewportDebugCommand);
        return;

    case studio_session::ViewportMode::System:
    case studio_session::ViewportMode::FlatMap:
    {
        const std::string_view id =
            expansion_.ControlledViewportId();
        if (session_->Viewports().Find(id) == nullptr)
        {
            throw std::logic_error(
                "Controlled viewport is unavailable.");
        }
        session_->Viewports().SetMode(id, mode);
        return;
    }
    }

    throw std::logic_error("Unsupported viewport mode.");
}

StudioViewportPanels::StudioViewportPanels(
    StudioRenderViewSet& views,
    studio_session::StudioSession& session) noexcept
{
    Rebind(views, session);
}

void StudioViewportPanels::Rebind(
    StudioRenderViewSet& views,
    studio_session::StudioSession& session)
{
    UnregisterViewportModeCommands();

    views_ = &views;
    session_ = &session;
    views_->CreateDefaults();
    status_.clear();
    terrainTool_ =
        StudioTerrainAuthoringTool::Select;
    terrainSplinePoints_.clear();
    terrainSplineTerrain_.reset();
}

void StudioViewportPanels::ClearBinding() noexcept
{
    UnregisterViewportModeCommands();

    views_ = nullptr;
    session_ = nullptr;
    status_.clear();
    terrainSplinePoints_.clear();
    terrainSplineTerrain_.reset();
}

void StudioViewportPanels::RegisterBase(
    editor_ui::EditorUi& ui)
{
    if (views_ != nullptr)
    {
        views_->CreateDefaults();
    }

    ui.RegisterPanel({
        .id = kPrimaryViewportPanel,
        .title = "Viewport",
        .defaultOpen = true,
        .defaultDock = orbit::editor_ui::DockRegion::Center,
        .dockOrder = 0,
        .minSize = {.width = 320.0F, .height = 200.0F},
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawViewBase(context, "studio.primary");
            }
    });

    ui.RegisterPanel({
        .id = kSecondaryViewportPanel,
        .title = "Body Map / Debug View",
        .defaultOpen = true,
        .defaultDock = orbit::editor_ui::DockRegion::Center,
        .dockOrder = 10,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawViewBase(context, "studio.map");
            }
    });
}

void StudioViewportPanels::RegisterSecondaryBase(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kSecondaryViewportPanel,
        .title = "Body Map / Debug View",
        .defaultOpen = true,
        .defaultDock = orbit::editor_ui::DockRegion::Center,
        .dockOrder = 10,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawViewBase(context, "studio.map");
            }
    });
}

void StudioViewportPanels::DrawViewBase(
    editor_ui::PanelContext& context,
    const std::string_view id)
{
    if (views_ == nullptr || session_ == nullptr)
    {
        context.Text("Open or create a project to activate this viewport.");
        return;
    }

    EnsureViewportModeCommands();

    auto* renderView = views_->Find(id);
    const auto* target = session_->Viewports().Find(id);

    if (renderView == nullptr || target == nullptr)
    {
        context.Text("Viewport is not registered.");
        return;
    }

    const auto queueTerrainAuthoringInvalidation =
        [this, id](
            const universe::BodyId body,
            const std::span<const math::Double3> points,
            const f64 influenceRadiusMeters,
            const u32 downstreamRadiusTiles = 2U,
            const terrain_dependency::TerrainChangeKind kind =
                terrain_dependency::TerrainChangeKind::TerrainAuthoring)
        {
            const auto planet =
                session_->World().
                    Surfaces().
                    Registry().
                    SphericalPlanetDefinition(body);

            const auto runtime =
                session_->TerrainRuntime().
                    Capture(id);

            if (!planet.has_value() ||
                !runtime.has_value() ||
                runtime->body != body)
            {
                throw std::runtime_error(
                    "Terrain invalidation requires a current spherical terrain viewport runtime.");
            }

            const auto requests =
                studio_session::
                    BuildTerrainAuthoringInvalidations(
                        *planet,
                        points,
                        influenceRadiusMeters,
                        runtime->physicalPageLevel,
                        downstreamRadiusTiles,
                        kind);

            session_->QueueTerrainInvalidations(requests);
        };

    const auto inspectSelectedBiomeAtPick =
        [this](const StudioSurfacePick& pick)
        {
            hoveredBiomeAuthoredWeight_.reset();
            hoveredBiomeAutomaticWeight_.reset();

            if (terrainTool_ !=
                StudioTerrainAuthoringTool::BiomePaint)
            {
                return;
            }

            const auto biomeObject =
                SelectedBiomeObject(*session_);

            if (!biomeObject.has_value())
            {
                return;
            }

            auto& surfaces =
                session_->World().Surfaces();

            const auto biomeId =
                surfaces.BiomeForObject(
                    *biomeObject);

            const auto planet =
                surfaces.Registry().
                    SphericalPlanetDefinition(
                        pick.body);

            const auto* services =
                surfaces.ServicesForBody(
                    pick.body);

            if (!biomeId.has_value() ||
                !planet.has_value() ||
                services == nullptr)
            {
                return;
            }

            const auto* biome =
                services->Biomes().Find(
                    *biomeId);

            if (biome == nullptr)
            {
                return;
            }

            hoveredBiomeAuthoredWeight_ =
                services->Biomes().
                    EvaluateAuthoredWeight(
                        *biome,
                        pick.surface.unitDirection,
                        planet->radiusMeters);

            if (!biomeAutomaticOverlay_)
            {
                return;
            }

            bool supported = true;

            for (const auto& selector :
                 biome->placement.selectors)
            {
                if (!selector.enabled)
                {
                    continue;
                }

                if (selector.field !=
                        terrain_biome::
                            BiomeSelectorField::Elevation &&
                    selector.field !=
                        terrain_biome::
                            BiomeSelectorField::Latitude)
                {
                    supported = false;
                    break;
                }
            }

            if (!supported)
            {
                return;
            }

            terrain_biome::BiomePlacementContext
                placement{};

            placement.unitDirection =
                pick.surface.unitDirection;
            placement.planetRadiusMeters =
                planet->radiusMeters;
            placement.elevationMeters =
                pick.physicalElevationMeters;
            placement.latitudeRadians =
                std::asin(
                    std::clamp(
                        pick.surface.
                            unitDirection.y,
                        -1.0,
                        1.0));

            hoveredBiomeAutomaticWeight_ =
                services->Biomes().
                    EvaluatePlacement(
                        *biome,
                        placement).
                    automaticWeight;
        };

    // Permanent view mode and presentation controls live in row 2. The
    // viewport body owns only the rendered scene and transient actions tied to
    // geometry currently being manipulated inside that scene.
    bool transientChrome = false;

    if (target->mode !=
            studio_session::ViewportMode::Debug &&
        IsSplineTool(terrainTool_) &&
        !terrainSplinePoints_.empty())
    {
        transientChrome = true;
        context.Text(
            std::format(
                "{} · {} pts",
                TerrainToolName(terrainTool_),
                terrainSplinePoints_.size()));
        context.SameLine();

        const std::string commitLabel =
            "Commit##terrain-spline-commit:" +
            std::string(id);
        const std::string cancelLabel =
            "Cancel##terrain-spline-cancel:" +
            std::string(id);

        if (context.Button(commitLabel))
        {
            if (terrainSplinePoints_.size() < 2U ||
                !terrainSplineTerrain_.has_value())
            {
                status_ =
                    "A terrain spline requires at least two picked control points.";
            }
            else
            {
                try
                {
                    editor_model::SurfaceAuthoringModel model(
                        session_->World().Objects(),
                        session_->World().Commands(),
                        session_->World().Selection());

                    const auto constraint =
                        terrainTool_ == StudioTerrainAuthoringTool::DrainagePath
                            ? model.AddDrainageSpline(
                                  *terrainSplineTerrain_,
                                  terrainSplinePoints_,
                                  terrainSplineHalfWidthMeters_,
                                  terrainSplineFalloffMeters_,
                                  terrainDrainageGuidance_)
                            : terrainTool_ == StudioTerrainAuthoringTool::Canyon
                                ? model.AddCanyonSpline(
                                      *terrainSplineTerrain_,
                                      terrainSplinePoints_,
                                      terrainSplineHalfWidthMeters_,
                                      terrainSplineFalloffMeters_,
                                      terrainSplineHeightMeters_)
                                : model.AddRidgeSpline(
                                      *terrainSplineTerrain_,
                                      terrainSplinePoints_,
                                      terrainSplineHalfWidthMeters_,
                                      terrainSplineFalloffMeters_,
                                      terrainSplineHeightMeters_);

                    model.SelectObject(constraint);

                    const auto body =
                        session_->World().
                            Surfaces().
                            BodyForTerrainObject(
                                *terrainSplineTerrain_);

                    if (!body.has_value())
                    {
                        throw std::runtime_error(
                            "Committed terrain spline lost its target body.");
                    }

                    queueTerrainAuthoringInvalidation(
                        *body,
                        terrainSplinePoints_,
                        terrainSplineHalfWidthMeters_ +
                            terrainSplineFalloffMeters_,
                        3U);

                    terrainSplinePoints_.clear();
                    terrainSplineTerrain_.reset();
                    views_->ClearTerrainAuthoringOverlay(id);
                    status_ = terrainTool_ == StudioTerrainAuthoringTool::DrainagePath
                        ? "Drainage guidance path committed with bounded M27 invalidation."
                        : "Terrain spline committed with bounded M27 invalidation.";
                }
                catch (const std::exception& exception)
                {
                    status_ = exception.what();
                }
            }
        }

        context.SameLine();
        if (context.Button(cancelLabel))
        {
            terrainSplinePoints_.clear();
            terrainSplineTerrain_.reset();
            views_->ClearTerrainAuthoringOverlay(id);
            status_ =
                "Transient terrain spline cancelled.";
        }
    }

    if (transientChrome)
    {
        context.Separator();
    }

    const auto available = context.ContentAvailable();
    const u32 panelWidth =
        static_cast<u32>(
            std::max(available.width, 1.0F));
    const u32 panelHeight =
        static_cast<u32>(
            std::max(available.height, 1.0F));
    const bool captureSize =
        id == "studio.primary" && viewportCaptureResolution_.has_value();
    const u32 width = captureSize
        ? viewportCaptureResolution_->first
        : panelWidth;
    const u32 height = captureSize
        ? viewportCaptureResolution_->second
        : panelHeight;

    if (renderView->Width() != width ||
        renderView->Height() != height)
    {
        views_->Resize(id, width, height);
        renderView = views_->Find(id);
    }

    const auto imageInteraction = captureSize
        ? context.ImageFit(
              renderView->DisplayColor(),
              available,
              {
                  .width = static_cast<f32>(renderView->Width()),
                  .height = static_cast<f32>(renderView->Height())})
        : context.Image(
              renderView->DisplayColor(),
              {
                  .width = static_cast<f32>(renderView->Width()),
                  .height = static_cast<f32>(renderView->Height())});

    textHud_.Draw(context, *views_, id, imageInteraction);

    const bool gizmoOwnsPointer =
        HandleViewportGizmo(context, id);

    if (target->mode ==
            studio_session::ViewportMode::Debug ||
        terrainTool_ ==
            StudioTerrainAuthoringTool::Select)
    {
        hoveredBiomeAuthoredWeight_.reset();
        hoveredBiomeAutomaticWeight_.reset();
        views_->ClearTerrainAuthoringOverlay(id);
    }
    else if (imageInteraction.hovered)
    {
        const auto hoverPick =
            views_->PickTerrainSurface(
                id,
                imageInteraction.u,
                imageInteraction.v);

        if (hoverPick.has_value())
        {
            inspectSelectedBiomeAtPick(
                *hoverPick);

            StudioTerrainAuthoringOverlay overlay{
                .body = hoverPick->body,
                .kind =
                    IsSplineTool(terrainTool_)
                        ? StudioTerrainOverlayKind::Spline
                        : StudioTerrainOverlayKind::Brush,
                .influenceRadiusMeters =
                    IsSplineTool(terrainTool_)
                        ? terrainSplineHalfWidthMeters_ +
                              terrainSplineFalloffMeters_
                        : terrainTool_ ==
                                  StudioTerrainAuthoringTool::BiomePaint
                            ? biomeBrushOuterRadiusMeters_
                            : terrainBrushOuterRadiusMeters_
            };

            if (IsSplineTool(terrainTool_))
            {
                overlay.controlUnitDirections =
                    terrainSplinePoints_;

                const auto direction =
                    hoverPick->surface.unitDirection;

                if (overlay.controlUnitDirections.empty() ||
                    math::Length(
                        overlay.controlUnitDirections.back() -
                        direction) > 1.0e-10)
                {
                    overlay.controlUnitDirections.push_back(
                        direction);
                }
            }
            else
            {
                overlay.controlUnitDirections.push_back(
                    hoverPick->surface.unitDirection);
            }

            views_->SetTerrainAuthoringOverlay(
                id,
                std::move(overlay));
        }
        else
        {
            hoveredBiomeAuthoredWeight_.reset();
            hoveredBiomeAutomaticWeight_.reset();
            views_->ClearTerrainAuthoringOverlay(id);
        }
    }
    else if (IsSplineTool(terrainTool_) &&
             !terrainSplinePoints_.empty() &&
             terrainSplineTerrain_.has_value())
    {
        const auto body =
            session_->World().Surfaces().
                BodyForTerrainObject(
                    *terrainSplineTerrain_);

        if (body.has_value())
        {
            views_->SetTerrainAuthoringOverlay(
                id,
                {
                    .body = *body,
                    .kind = StudioTerrainOverlayKind::Spline,
                    .controlUnitDirections = terrainSplinePoints_,
                    .influenceRadiusMeters =
                        terrainSplineHalfWidthMeters_ +
                        terrainSplineFalloffMeters_
                });
        }
        else
        {
            views_->ClearTerrainAuthoringOverlay(id);
        }
    }
    else
    {
        if (terrainTool_ ==
            StudioTerrainAuthoringTool::BiomePaint)
        {
            hoveredBiomeAuthoredWeight_.reset();
            hoveredBiomeAutomaticWeight_.reset();
        }

        views_->ClearTerrainAuthoringOverlay(id);
    }

    if (imageInteraction.clicked && !gizmoOwnsPointer)
    {
        if (target->mode ==
            studio_session::ViewportMode::Debug)
        {
            if (views_->SelectDebugPhysicalPage(
                    id,
                    imageInteraction.u,
                    imageInteraction.v))
            {
                const auto page =
                    views_->DebugPhysicalPage(id);
                if (page.has_value())
                {
                    const auto& tile =
                        page->address.tile;
                    status_ =
                        std::format(
                            "Selected physical page {} L{} ({}, {}).",
                            CubeFaceName(tile.face),
                            tile.level,
                            tile.x,
                            tile.y);
                }
            }
            else
            {
                status_ =
                    "Debug click did not intersect production terrain.";
            }
        }
        else
        {
            const auto pick =
                views_->PickTerrainSurface(
                    id,
                    imageInteraction.u,
                    imageInteraction.v);

            if (!pick.has_value())
            {
                status_ =
                    "Viewport click did not intersect production terrain.";
            }
            else if (
                terrainTool_ ==
                StudioTerrainAuthoringTool::Select)
            {
                const std::array<
                    scene::ObjectId,
                    1U>
                    selectedObjects{
                        pick->semanticBody
                    };

                session_->World().
                    Selection().Set(
                        selectedObjects);

                status_ =
                    std::format(
                        "Terrain pick: elevation {:.2f} m | dir [{:.6f}, {:.6f}, {:.6f}].",
                        pick->
                            physicalElevationMeters,
                        pick->
                            surface.unitDirection.x,
                        pick->
                            surface.unitDirection.y,
                        pick->
                            surface.unitDirection.z);
            }
            else
            {
                const auto terrainObject =
                    session_->World().
                        Surfaces().
                        TerrainObjectForBody(
                            pick->body);

                if (!terrainObject.has_value())
                {
                    status_ =
                        "Picked body has no semantic Terrain Surface.";
                }
                else if (terrainTool_ ==
                         StudioTerrainAuthoringTool::BiomePaint)
                {
                    const auto biomeObject =
                        SelectedBiomeObject(*session_);

                    if (!biomeObject.has_value())
                    {
                        status_ =
                            "Select a Biome or one of its authored children before painting.";
                    }
                    else
                    {
                        try
                        {
                            editor_model::
                                SurfaceAuthoringModel
                                model(
                                    session_->World().
                                        Objects(),
                                    session_->World().
                                        Commands(),
                                    session_->World().
                                        Selection());

                            const auto rocky =
                                model.SelectedRockyBody();

                            if (!rocky.has_value() ||
                                rocky->terrain !=
                                    *terrainObject)
                            {
                                throw std::runtime_error(
                                    "Selected biome belongs to a different terrain body.");
                            }

                            const auto mask =
                                model.PaintBiomeMask(
                                    *biomeObject,
                                    biomePaintOperation_,
                                    pick->surface.
                                        unitDirection,
                                    biomeBrushInnerRadiusMeters_,
                                    biomeBrushOuterRadiusMeters_,
                                    biomeBrushValue_,
                                    biomeBrushOpacity_);

                            model.SelectObject(mask);

                            const math::Double3 point =
                                pick->surface.
                                    unitDirection;

                            queueTerrainAuthoringInvalidation(
                                pick->body,
                                std::span{
                                    &point,
                                    std::size_t{1U}},
                                biomeBrushOuterRadiusMeters_,
                                0U,
                                terrain_dependency::
                                    TerrainChangeKind::
                                        BiomePlacement);

                            inspectSelectedBiomeAtPick(
                                *pick);

                            status_ =
                                std::format(
                                    "Biome {} mask committed; only M27 biome descendants were invalidated.",
                                    BiomeOperationName(
                                        biomePaintOperation_));
                        }
                        catch (const std::exception& exception)
                        {
                            status_ =
                                exception.what();
                        }
                    }
                }
                else if (IsSplineTool(
                             terrainTool_))
                {
                    if (terrainSplineTerrain_ !=
                        terrainObject)
                    {
                        terrainSplinePoints_.clear();
                        terrainSplineTerrain_ =
                            terrainObject;
                    }

                    const auto direction =
                        pick->surface.
                            unitDirection;

                    if (terrainSplinePoints_.empty() ||
                        math::Length(
                            terrainSplinePoints_.back() -
                            direction) >
                            1.0e-10)
                    {
                        terrainSplinePoints_.
                            push_back(
                                direction);
                    }

                    status_ =
                        std::format(
                            "{} spline point {} picked.",
                            TerrainToolName(
                                terrainTool_),
                            terrainSplinePoints_.
                                size());

                    if (imageInteraction.doubleClicked &&
                        terrainSplinePoints_.size() >=
                            2U)
                    {
                        try
                        {
                            editor_model::
                                SurfaceAuthoringModel
                                model(
                                    session_->World().
                                        Objects(),
                                    session_->World().
                                        Commands(),
                                    session_->World().
                                        Selection());

                            const auto constraint =
                                terrainTool_ == StudioTerrainAuthoringTool::DrainagePath
                                    ? model.AddDrainageSpline(
                                          *terrainSplineTerrain_,
                                          terrainSplinePoints_,
                                          terrainSplineHalfWidthMeters_,
                                          terrainSplineFalloffMeters_,
                                          terrainDrainageGuidance_)
                                    : terrainTool_ == StudioTerrainAuthoringTool::Canyon
                                        ? model.AddCanyonSpline(
                                              *terrainSplineTerrain_,
                                              terrainSplinePoints_,
                                              terrainSplineHalfWidthMeters_,
                                              terrainSplineFalloffMeters_,
                                              terrainSplineHeightMeters_)
                                        : model.AddRidgeSpline(
                                              *terrainSplineTerrain_,
                                              terrainSplinePoints_,
                                              terrainSplineHalfWidthMeters_,
                                              terrainSplineFalloffMeters_,
                                              terrainSplineHeightMeters_);

                            model.SelectObject(
                                constraint);

                            queueTerrainAuthoringInvalidation(
                                pick->body,
                                terrainSplinePoints_,
                                terrainSplineHalfWidthMeters_ +
                                    terrainSplineFalloffMeters_,
                                3U);

                            terrainSplinePoints_.
                                clear();
                            terrainSplineTerrain_.
                                reset();
                            status_ = terrainTool_ == StudioTerrainAuthoringTool::DrainagePath
                                ? "Drainage guidance path committed with bounded M27 invalidation."
                                : "Terrain spline committed with bounded M27 invalidation.";
                        }
                        catch (const std::exception& exception)
                        {
                            status_ =
                                exception.what();
                        }
                    }
                }
                else
                {
                    try
                    {
                        editor_model::
                            SurfaceAuthoringModel
                            model(
                                session_->World().
                                    Objects(),
                                session_->World().
                                    Commands(),
                                session_->World().
                                    Selection());

                        scene::ObjectId constraint{};

                        switch (terrainTool_)
                        {
                        case StudioTerrainAuthoringTool::Raise:
                            constraint =
                                model.AddHeightBrush(
                                    *terrainObject,
                                    pick->surface.
                                        unitDirection,
                                    terrainBrushInnerRadiusMeters_,
                                    terrainBrushOuterRadiusMeters_,
                                    std::abs(
                                        terrainBrushHeightMeters_));
                            break;

                        case StudioTerrainAuthoringTool::Lower:
                            constraint =
                                model.AddHeightBrush(
                                    *terrainObject,
                                    pick->surface.
                                        unitDirection,
                                    terrainBrushInnerRadiusMeters_,
                                    terrainBrushOuterRadiusMeters_,
                                    -std::abs(
                                        terrainBrushHeightMeters_));
                            break;

                        case StudioTerrainAuthoringTool::Protection:
                            constraint =
                                model.AddProtectionBrush(
                                    *terrainObject,
                                    pick->surface.
                                        unitDirection,
                                    terrainBrushInnerRadiusMeters_,
                                    terrainBrushOuterRadiusMeters_,
                                    terrainProtection_);
                            break;

                        case StudioTerrainAuthoringTool::Drainage:
                            constraint =
                                model.AddDrainageBrush(
                                    *terrainObject,
                                    pick->surface.
                                        unitDirection,
                                    terrainBrushInnerRadiusMeters_,
                                    terrainBrushOuterRadiusMeters_,
                                    terrainDrainageGuidance_);
                            break;

                        case StudioTerrainAuthoringTool::Material:
                        {
                            const auto* services =
                                session_->World().
                                    Surfaces().
                                    ServicesForBody(
                                        pick->body);

                            if (services == nullptr)
                            {
                                throw std::runtime_error(
                                    "TerrainBodyServices are unavailable for geology override.");
                            }

                            constraint =
                                model.AddMaterialBrush(
                                    *terrainObject,
                                    pick->surface.
                                        unitDirection,
                                    terrainBrushInnerRadiusMeters_,
                                    terrainBrushOuterRadiusMeters_,
                                    services->
                                        DefaultBedrock(),
                                    1.0);
                            break;
                        }

                        case StudioTerrainAuthoringTool::Select:
                        case StudioTerrainAuthoringTool::DrainagePath:
                        case StudioTerrainAuthoringTool::Canyon:
                        case StudioTerrainAuthoringTool::Ridge:
                        case StudioTerrainAuthoringTool::BiomePaint:
                            break;
                        }

                        if (constraint.IsValid())
                        {
                            model.SelectObject(
                                constraint);

                            const math::Double3 point =
                                pick->surface.unitDirection;

                            queueTerrainAuthoringInvalidation(
                                pick->body,
                                std::span{
                                    &point,
                                    std::size_t{1U}},
                                terrainBrushOuterRadiusMeters_,
                                terrainTool_ ==
                                        StudioTerrainAuthoringTool::Drainage
                                    ? 3U
                                    : 2U);

                            status_ =
                                std::string(
                                    TerrainToolName(
                                        terrainTool_)) +
                                " constraint committed with bounded M27 invalidation.";
                        }
                    }
                    catch (const std::exception& exception)
                    {
                        status_ =
                            exception.what();
                    }
                }
            }
        }
    }
}
} // namespace orbit::studio_ui

namespace orbit::studio_ui
{
namespace
{
using WorkspaceMode = StudioWorkspaceMode;
using BrowserMode = StudioBrowserMode;

editor_ui::EditorUi* g_workspaceUi = nullptr;
StudioViewportPanels* g_shellPanels = nullptr;
WorkspaceMode g_workspaceMode = WorkspaceMode::Scene;
BrowserMode g_browserMode = BrowserMode::World;

void ClosePanels(
    editor_ui::EditorUi& ui,
    const std::initializer_list<std::string_view> titles)
{
    for (const std::string_view title : titles)
    {
        static_cast<void>(
            ui.ClosePanelByTitle(title));
    }
}

void OpenPanels(
    editor_ui::EditorUi& ui,
    const std::initializer_list<std::string_view> titles)
{
    for (const std::string_view title : titles)
    {
        static_cast<void>(
            ui.FocusPanelByTitle(title));
    }
}

void CloseLegacyBrowserSources(editor_ui::EditorUi& ui)
{
    ClosePanels(
        ui,
        {
            "Explorer Source",
            "Material Service"
        });
}

void CloseSpecialistPanels(editor_ui::EditorUi& ui)
{
    ClosePanels(
        ui,
        {
            "Inspector",
            "Body Map / Debug View",
            "System View",
            "Shading",
            "Shading Materials",
            "Celestial",
            "Surface Authoring",
            "Volumes",
            "Debug",
            "Display Diagnostics",
            "World Documents",
            "Project Settings",
            "Planning",
            "Plugins"
        });
}

void OpenCanonicalBrowser(
    editor_ui::EditorUi& ui,
    const BrowserMode mode)
{
    g_browserMode = mode;
    CloseLegacyBrowserSources(ui);
    OpenPanels(ui, {"Explorer"});
}

void OpenCanonicalWorkspace(
    editor_ui::EditorUi& ui,
    const std::initializer_list<std::string_view> centerPanels)
{
    CloseSpecialistPanels(ui);
    ClosePanels(
        ui,
        {
            "Build",
            "Output"
        });

    OpenCanonicalBrowser(ui, g_browserMode);
    OpenPanels(ui, centerPanels);
    OpenPanels(ui, {"Properties"});
}

void ActivateWorkspace(
    editor_ui::EditorUi& ui,
    const WorkspaceMode mode)
{
    g_workspaceMode = mode;
    g_browserMode = DefaultBrowserMode(mode);
    if (g_shellPanels != nullptr)
    {
        g_shellPanels->SyncModeToolbar();
    }

    switch (mode)
    {
    case WorkspaceMode::Scene:
    case WorkspaceMode::Planet:
    case WorkspaceMode::Celestial:
    case WorkspaceMode::Simulation:
        OpenCanonicalWorkspace(
            ui,
            {"Viewport"});
        break;

    case WorkspaceMode::Shading:
        CloseSpecialistPanels(ui);
        ClosePanels(
            ui,
            {
                "Viewport",
                "Build",
                "Output"
            });
        OpenCanonicalBrowser(ui, BrowserMode::Assets);
        OpenPanels(
            ui,
            {"Shading", "Properties"});
        break;

    case WorkspaceMode::Planning:
        CloseSpecialistPanels(ui);
        ClosePanels(ui, {"Viewport", "Build", "Output"});
        OpenCanonicalBrowser(ui, BrowserMode::World);
        OpenPanels(ui, {"Planning", "Properties"});
        break;

    case WorkspaceMode::Plugins:
        CloseSpecialistPanels(ui);
        ClosePanels(ui, {"Viewport", "Build", "Output"});
        OpenCanonicalBrowser(ui, BrowserMode::Assets);
        OpenPanels(ui, {"Plugins", "Properties"});
        break;
    }
}

void RegisterWorkspaceActions(editor_ui::EditorUi& ui)
{
    if (g_workspaceUi == &ui)
    {
        return;
    }

    g_workspaceUi = &ui;

    ui.RegisterMenuAction({
        .menu = "Home",
        .label = "Workspace: Build",
        .invoke = [&ui]
        {
            ActivateWorkspace(ui, WorkspaceMode::Scene);
        }
    });

    ui.RegisterMenuAction({
        .menu = "Home",
        .label = "Workspace: Planet",
        .invoke = [&ui]
        {
            ActivateWorkspace(ui, WorkspaceMode::Planet);
        }
    });

    ui.RegisterMenuAction({
        .menu = "Home",
        .label = "Workspace: Universe",
        .invoke = [&ui]
        {
            ActivateWorkspace(ui, WorkspaceMode::Celestial);
        }
    });

    ui.RegisterMenuAction({
        .menu = "Home",
        .label = "Workspace: Simulation",
        .invoke = [&ui]
        {
            ActivateWorkspace(ui, WorkspaceMode::Simulation);
        }
    });

    ui.RegisterMenuAction({
        .menu = "Home",
        .label = "Workspace: Shading",
        .invoke = [&ui]
        {
            ActivateWorkspace(ui, WorkspaceMode::Shading);
        }
    });

    ui.RegisterMenuAction({
        .menu = "Home",
        .label = "Workspace: Planning",
        .invoke = [&ui] { ActivateWorkspace(ui, WorkspaceMode::Planning); }
    });

    ui.RegisterMenuAction({
        .menu = "Home",
        .label = "Workspace: Plugins",
        .invoke = [&ui] { ActivateWorkspace(ui, WorkspaceMode::Plugins); }
    });
}

void RegisterWorldAssetsBrowser(editor_ui::EditorUi& ui)
{
    const auto& contract =
        kWorldAssetsBrowserContract;

    if (ui.HasPanel(contract.panel))
    {
        return;
    }

    ui.RegisterPanel({
        .id = contract.panel,
        .title = "Explorer",
        .defaultOpen = contract.defaultOpen,
        .defaultDock = contract.defaultDock,
        .dockOrder = contract.dockOrder,
        .minSize = contract.minSize,
        .defaultSize = contract.defaultSize,
        .draw =
            [&ui](editor_ui::PanelContext& context)
            {
                CloseLegacyBrowserSources(ui);
                if (!ui.DrawPanelContentsByTitle(
                    "Explorer Source",
                    context))
                {
                    context.MutedText(
                        "Explorer is not registered in this Studio configuration.");
                }
            }
    });
}

[[nodiscard]] const char* WorkspaceName(
    const WorkspaceMode mode) noexcept
{
    return StudioWorkspaceName(mode).data();
}
} // namespace

StudioViewportPanels::StudioViewportPanels() = default;

StudioViewportPanels::~StudioViewportPanels()
{
    if (g_shellPanels == this)
    {
        static_cast<void>(
            editor_ui::RemoveShellBand("orbit.activity"));
        static_cast<void>(
            editor_ui::RemoveShellBand("orbit.workspace-tabs"));
        static_cast<void>(
            editor_ui::RemoveShellBand("orbit.mode-toolbar"));
        g_shellPanels = nullptr;
        g_workspaceUi = nullptr;
    }
}

void StudioViewportPanels::RegisterShellBands(
    editor_ui::EditorUi& ui)
{
    g_shellPanels = this;
    g_workspaceUi = &ui;

    editor_ui::UpsertShellBand({
        .id = "orbit.workspace-tabs",
        .order = -10,
        .height = 58.0F,
        .emphasized = true,
        .draw = [](editor_ui::PanelContext& context)
        {
            if (g_shellPanels != nullptr) g_shellPanels->DrawWorkspaceBand(context);
        }
    });

    editor_ui::UpsertShellBand({
        .id = "orbit.activity",
        .order = 0,
        .height = 34.0F,
        .edge = editor_ui::ShellBandEdge::Bottom,
        .draw =
            [](editor_ui::PanelContext& context)
            {
                if (g_shellPanels != nullptr)
                {
                    g_shellPanels->DrawActivityBand(context);
                }
            }
    });

    SyncModeToolbar();
}

std::string_view StudioViewportPanels::WorkspaceModeName() const noexcept
{
    return StudioWorkspaceName(g_workspaceMode);
}

bool StudioViewportPanels::SetWorkspaceMode(const std::string_view name)
{
    const auto parsed = ParseWorkspaceMode(name);
    if (!parsed.has_value() || g_workspaceUi == nullptr)
    {
        return false;
    }

    ActivateWorkspace(*g_workspaceUi, *parsed);
    return true;
}

void StudioViewportPanels::SyncModeToolbar()
{
    if (g_workspaceMode != WorkspaceMode::Scene &&
        g_workspaceMode != WorkspaceMode::Planet &&
        g_workspaceMode != WorkspaceMode::Celestial)
    {
        static_cast<void>(
            editor_ui::RemoveShellBand("orbit.mode-toolbar"));
        return;
    }

    editor_ui::UpsertShellBand({
        .id = "orbit.mode-toolbar",
        .order = 5,
        .height = 44.0F,
        .draw =
            [](editor_ui::PanelContext& context)
            {
                if (g_shellPanels == nullptr)
                {
                    return;
                }

                if (g_workspaceMode == WorkspaceMode::Planet)
                {
                    g_shellPanels->DrawPlanetToolbar(context);
                }
                else if (g_workspaceMode == WorkspaceMode::Celestial)
                {
                    g_shellPanels->DrawCelestialToolbar(context);
                }
                else
                {
                    g_shellPanels->DrawSceneToolbar(context);
                }
            }
    });
}

void StudioViewportPanels::DrawElementBubble(
    editor_ui::PanelContext& context,
    const scene::ObjectId object)
{
    auto& world = session_->World();
    const auto record = world.Objects().Find(object);
    if (!record.has_value())
    {
        return;
    }

    const std::string suffix = object.ToString();
    bool open =
        context.ToolbarButton("Properties##bubble-open-" + suffix, editor_ui::ToolbarIcon::Properties, false, true, true);
    context.AnchorNextPopupBelowItem();

    if (bubbleOpenRequest_.has_value() &&
        *bubbleOpenRequest_ == object)
    {
        bubbleOpenRequest_.reset();
        open = true;
    }

    if (!context.BeginPopup(
            "element-bubble##" + suffix,
            open,
            {340.0F, 0.0F}))
    {
        return;
    }

    const auto* type = world.Schemas().FindType(record->type);
    context.Heading(
        record->name +
        (type != nullptr
             ? "  -  " + type->displayName
             : std::string{}));

    std::size_t advancedCount = 0U;

    if (type != nullptr)
    {
        for (const auto& property : type->properties)
        {
            if (property.readOnly)
            {
                continue;
            }

            if (property.advanced)
            {
                ++advancedCount;
                continue;
            }

            const auto stored =
                world.Objects().GetProperty(object, property.id);
            const schema::PropertyValue current =
                stored.value_or(property.defaultValue);

            std::string label = property.name;
            if (!property.unit.empty())
            {
                label += " (" + property.unit + ")";
            }
            label += "##bubble-" + suffix + "-" + property.id.ToString();

            std::optional<schema::PropertyValue> next;

            if (const auto* asBool = std::get_if<bool>(&current))
            {
                bool edited = *asBool;
                if (context.Checkbox(label, edited))
                {
                    next = edited;
                }
            }
            else if (const auto* asInt = std::get_if<i64>(&current))
            {
                i64 edited = *asInt;
                if (context.InputInteger(label, edited))
                {
                    next = edited;
                }
            }
            else if (const auto* asFloat = std::get_if<f64>(&current))
            {
                f64 edited = *asFloat;
                const bool ranged =
                    property.range.minimum.has_value() &&
                    property.range.maximum.has_value();
                const bool changed = ranged
                    ? context.SliderDouble(
                          label,
                          edited,
                          *property.range.minimum,
                          *property.range.maximum)
                    : context.InputDouble(label, edited);
                if (changed)
                {
                    next = edited;
                }
            }
            else if (const auto* asText =
                         std::get_if<std::string>(&current))
            {
                std::string edited = *asText;
                if (context.InputText(label, edited))
                {
                    next = std::move(edited);
                }
            }
            else if (const auto* asVec =
                         std::get_if<math::Double3>(&current))
            {
                math::Double3 edited = *asVec;
                if (context.InputDouble3(label, edited))
                {
                    next = edited;
                }
            }
            else
            {
                context.MutedText(property.name + ": object reference");
            }

            if (next.has_value())
            {
                try
                {
                    world.Commands().SetProperty(
                        object,
                        property.id,
                        *next);
                    status_.clear();
                }
                catch (const std::exception& exception)
                {
                    status_ = exception.what();
                }
            }
        }
    }

    context.Separator();
    if (advancedCount > 0U)
    {
        context.MutedText(
            std::format(
                "{} advanced field{} in Properties.",
                advancedCount,
                advancedCount == 1U ? "" : "s"));
    }

    if (context.Button("Open in Properties##bubble-props-" + suffix))
    {
        const std::array selected{object};
        world.Selection().Set(std::span(selected));
        if (g_workspaceUi != nullptr)
        {
            static_cast<void>(
                g_workspaceUi->FocusPanelByTitle("Properties"));
        }
        context.CloseCurrentPopup();
    }

    context.EndPopup();
}

void StudioViewportPanels::DrawCelestialToolbar(
    editor_ui::PanelContext& context)
{
    if (session_ == nullptr ||
        !session_->World().HasWorld())
    {
        context.MutedText(
            "Open a world to use the Celestial tools.");
        return;
    }

    auto& world = session_->World();
    editor_model::CelestialAuthoringModel model(
        world.Objects(),
        world.Schemas(),
        world.Commands(),
        world.Selection());

    using editor_ui::ToolbarIcon;
    const bool compact = context.ContentAvailable().width < 1600.0F * editor_ui::CurrentUiScale();
    context.MutedText("Bodies");
    context.SameLine();

    const auto body = model.SelectedBody();
    if (!body.has_value())
    {
        context.MutedText(
            "Select a body (or one of its parts) to enable or disable its elements.");
        return;
    }

    context.Text(body->name);
    context.SameLine();
    DrawElementBubble(context, body->id);
    context.ToolbarDivider();

    const auto toggle =
        [&](const schema::TypeId type,
            const std::string_view label,
            const ToolbarIcon icon)
        {
            const auto state =
                model.CapabilityEnabled(body->id, type);
            bool enabled = state.value_or(false);

            const std::string id =
                std::string(label) +
                "##celestial-tb-" +
                type.ToString();

            if (context.ToolbarButton(id, icon, enabled, true, compact))
            {
                enabled = !enabled;
                try
                {
                    model.SetCapabilityEnabled(
                        body->id,
                        type,
                        enabled);
                    status_.clear();
                }
                catch (const std::exception& exception)
                {
                    status_ = exception.what();
                }
            }

            if (state.has_value())
            {
                for (const auto& child :
                     world.Objects().Children(body->id))
                {
                    if (child.type == type)
                    {
                        context.SameLine();
                        DrawElementBubble(context, child.id);
                        break;
                    }
                }
            }
            context.SameLine();
        };

    toggle(world_model::kAtmosphereCapabilityType, "Atmosphere", ToolbarIcon::Atmosphere);
    toggle(world_model::kCloudLayerCapabilityType, "Clouds", ToolbarIcon::Clouds);
    toggle(world_model::kOceanCapabilityType, "Ocean", ToolbarIcon::Ocean);
    toggle(world_model::kRingSystemCapabilityType, "Rings", ToolbarIcon::Rings);
    toggle(world_model::kMagnetosphereCapabilityType, "Aurora", ToolbarIcon::Aurora);
    toggle(world_model::kSurfaceCapabilityType, "Surface", ToolbarIcon::Surface);

    if (context.ToolbarButton("More##celestial-tb-more", ToolbarIcon::More, false, true, compact))
    {
        celestialMoreRequested_ = true;
    }

    if (context.BeginPopup(
            "celestial-tb-more-popup",
            celestialMoreRequested_))
    {
        celestialMoreRequested_ = false;
        context.MutedText("All elements of this body");
        context.Separator();

        for (const auto& capability :
             model.AvailableCapabilities())
        {
            bool enabled =
                model.CapabilityEnabled(
                        body->id,
                        capability.type)
                    .value_or(false);

            if (context.Checkbox(
                    std::string(capability.label) +
                        "##celestial-more-" +
                        capability.type.ToString(),
                    enabled))
            {
                try
                {
                    model.SetCapabilityEnabled(
                        body->id,
                        capability.type,
                        enabled);
                    status_.clear();
                }
                catch (const std::exception& exception)
                {
                    status_ = exception.what();
                }
            }
        }

        context.EndPopup();
    }

    context.ToolbarDivider();
    if (context.ToolbarButton("Commands /##toolbar-commands", editor_ui::ToolbarIcon::More, false, true, true))
        expansion_.RequestCommandPaletteOpen();

    if (!status_.empty())
    {
        context.SameLine();
        context.MutedText(status_);
    }
}

bool StudioViewportPanels::FrameSelectedObject()
{
    if (session_ == nullptr || views_ == nullptr ||
        !session_->World().HasWorld())
    {
        return false;
    }
    std::string_view id = expansion_.ControlledViewportId();
    const auto* viewport = session_->Viewports().Find(id);
    if (viewport == nullptr ||
        viewport->mode != studio_session::ViewportMode::Perspective)
    {
        id = "studio.primary";
        viewport = session_->Viewports().Find(id);
    }
    if (viewport == nullptr || !viewport->target.has_value() ||
        viewport->mode != studio_session::ViewportMode::Perspective)
    {
        return false;
    }
    auto& world = session_->World();
    const auto& selected = world.Selection().Ordered();
    if (selected.empty())
    {
        return false;
    }
    if (selected.size() == 1U &&
        selected.front() == viewport->target->semanticObject)
    {
        return views_->FocusTerrainBody(id);
    }

    std::optional<math::Double3> boundsMin;
    math::Double3 boundsMax{};
    const auto includeSphere = [&](const math::Double3& position,
                                   const f64 radius)
    {
        const math::Double3 extent{radius, radius, radius};
        const auto low = position - extent;
        const auto high = position + extent;
        if (!boundsMin.has_value())
        {
            boundsMin = low;
            boundsMax = high;
        }
        else
        {
            boundsMin = {
                std::min(boundsMin->x, low.x),
                std::min(boundsMin->y, low.y),
                std::min(boundsMin->z, low.z)};
            boundsMax = {
                std::max(boundsMax.x, high.x),
                std::max(boundsMax.y, high.y),
                std::max(boundsMax.z, high.z)};
        }
    };
    for (const auto object : selected)
    {
        const auto body = session_->ActiveBody().Resolve(object);
        if (!body.has_value() ||
            body->body != viewport->target->body)
        {
            continue;
        }
        for (const auto& proxy : world_model::ResolveVisibilityProxies(
                 world.Objects(), object))
        {
            const f64 radius = proxy.shape ==
                    world_model::ResolvedVisibilityProxyShape::Box
                ? math::Length(proxy.halfExtentsMeters)
                : proxy.radiusMeters;
            includeSphere(proxy.positionMeters, radius);
        }
        for (const auto& light : world_model::ResolveAuthoredLocalLights(
                 world.Objects(), object))
        {
            includeSphere(light.positionMeters, 0.5);
        }
    }

    if (boundsMin.has_value())
    {
        const auto center = (*boundsMin + boundsMax) * 0.5;
        const f64 radius = std::max(
            math::Length(boundsMax - center), 0.5);
        return views_->FrameSelectedBounds(id, center, radius);
    }
    return false;
}

GizmoSettings StudioViewportPanels::Snapping() const noexcept
{
    return expansion_.ViewportState().gizmo;
}

void StudioViewportPanels::SetSnapping(const GizmoSettings& settings)
{
    if (!std::isfinite(settings.translationSnapMeters) || settings.translationSnapMeters <= 0 ||
        !std::isfinite(settings.rotationSnapDegrees) || settings.rotationSnapDegrees <= 0 ||
        !std::isfinite(settings.scaleSnapStep) || settings.scaleSnapStep <= 0 ||
        static_cast<u32>(settings.translationSnapUnit) > 5U)
        throw std::invalid_argument("Snap steps must be finite and greater than zero; distance units must be mm, cm, m, km, in, or ft.");
    auto& gizmo = expansion_.ViewportState().gizmo;
    gizmo.translationSnap = settings.translationSnap;
    gizmo.translationSnapMeters = settings.translationSnapMeters;
    gizmo.translationSnapUnit = settings.translationSnapUnit;
    gizmo.surfaceSnap = settings.surfaceSnap;
    gizmo.rotationSnap = settings.rotationSnap;
    gizmo.rotationSnapDegrees = settings.rotationSnapDegrees;
    gizmo.scaleSnap = settings.scaleSnap;
    gizmo.scaleSnapStep = settings.scaleSnapStep;
}

void StudioViewportPanels::DrawSnappingSettings(editor_ui::PanelContext& context)
{
    auto next = Snapping();
    bool changed = context.Checkbox("Grid snap##snap-grid",next.translationSnap);
    f64 distance = next.translationSnapMeters / SnapUnitMeters(next.translationSnapUnit);
    if (context.InputDouble("Distance##snap-distance",distance,130.0F * editor_ui::CurrentUiScale()))
    {
        next.translationSnapMeters = distance * SnapUnitMeters(next.translationSnapUnit);
        changed = true;
    }
    context.SameLine();
    static constexpr std::array<std::string_view,6> units{"mm","cm","m","km","in","ft"};
    i32 unit = static_cast<i32>(next.translationSnapUnit);
    if (context.Combo("##snap-distance-unit",units,unit,70.0F * editor_ui::CurrentUiScale()))
    {
        next.translationSnapUnit = static_cast<SnapLengthUnit>(unit);
        changed = true;
    }
    changed = context.Checkbox("Snap to surface##snap-surface",next.surfaceSnap) || changed;
    context.Separator();
    changed = context.Checkbox("Angle snap##snap-angle",next.rotationSnap) || changed;
    changed = context.InputDouble("Angle (deg)##snap-degrees",next.rotationSnapDegrees) || changed;
    context.Separator();
    changed = context.Checkbox("Scale snap##snap-scale",next.scaleSnap) || changed;
    f64 percent = next.scaleSnapStep * 100.0;
    if (context.InputDouble("Scale (%)##snap-percent",percent))
    {
        next.scaleSnapStep = percent / 100.0;
        changed = true;
    }
    if (changed)
    {
        try { SetSnapping(next); status_.clear(); }
        catch (const std::exception& e) { status_=e.what(); }
    }
    if (!status_.empty()) context.ErrorText(status_);
}

void StudioViewportPanels::DrawSnappingControls(editor_ui::PanelContext& context, bool compact)
{
    const auto settings = Snapping();
    bool enabled = settings.translationSnap || settings.surfaceSnap;
    std::string label = compact ? "Snap" : std::format("Snap {:g} {}",
        settings.translationSnapMeters / SnapUnitMeters(settings.translationSnapUnit),SnapUnitSymbol(settings.translationSnapUnit));
    if (settings.tool == GizmoTool::Rotate)
    {
        enabled = settings.rotationSnap;
        if (!compact) label = std::format("Snap {:g} deg",settings.rotationSnapDegrees);
    }
    else if (settings.tool == GizmoTool::Scale)
    {
        enabled = settings.scaleSnap;
        if (!compact) label = std::format("Snap {:g}%",settings.scaleSnapStep * 100.0);
    }
    // Retain the word Snap even in compact mode so its settings are discoverable.
    const bool open = context.ToolbarButton(label+"##toolbar-snap",editor_ui::ToolbarIcon::Snap,
        enabled,true,false,editor_ui::ToolbarStyle::ModifierMenu);
    context.AnchorNextPopupBelowItem();
    if (context.BeginPopup("toolbar-snap-settings",open,{360.0F * editor_ui::CurrentUiScale(),0}))
    {
        context.Heading("Snapping");
        context.MutedText("Movement uses real distances; default unit is meters.");
        DrawSnappingSettings(context);
        context.EndPopup();
    }
}

void StudioViewportPanels::DrawSceneToolbar(
    editor_ui::PanelContext& context)
{
    if (session_ == nullptr ||
        !session_->World().HasWorld())
    {
        context.MutedText(
            "Open a world to use the Build tools.");
        return;
    }

    auto& world = session_->World();
    auto& gizmo = expansion_.ViewportState().gizmo;
    constexpr std::string_view kViewport = "studio.primary";

    const auto run =
        [this](const auto& action)
        {
            try
            {
                action();
                status_.clear();
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
            }
        };

    const auto separator =
        [&context]
        {
            context.ToolbarDivider();
        };

    using editor_ui::ToolbarIcon;
    const bool compact = context.ContentAvailable().width < 1900.0F * editor_ui::CurrentUiScale();
    static constexpr std::array<editor_ui::ToolbarChoice, 4> kTools{{
        {"Select", ToolbarIcon::Select}, {"Move", ToolbarIcon::Move},
        {"Rotate", ToolbarIcon::Rotate}, {"Scale", ToolbarIcon::Scale}}};
    i32 tool = static_cast<i32>(gizmo.tool);
    if (context.ToolbarChoices(
            "scene-toolbar-gizmo-tool",
            kTools,
            tool, compact))
    {
        gizmo.tool = static_cast<GizmoTool>(std::clamp(tool, 0, 3));
    }

    context.SameLine();
    static constexpr std::array<editor_ui::ToolbarChoice, 2> kSpaces{{
        {"World", ToolbarIcon::World}, {"Local", ToolbarIcon::Local}}};
    i32 space = static_cast<i32>(gizmo.space);
    if (context.ToolbarChoices(
            "scene-toolbar-space",
            kSpaces,
            space, compact, editor_ui::ToolbarStyle::Modifier))
    {
        gizmo.space = static_cast<GizmoSpace>(std::clamp(space, 0, 1));
    }

    context.SameLine();
    DrawSnappingControls(context, compact);

    separator();
    expansion_.DrawCreationMenus(context, compact);

    separator();
    const bool hasSelection = !world.Selection().Ordered().empty();
    if (context.ToolbarButton("Frame Selected##scene-tb-frame", ToolbarIcon::Frame, false, hasSelection, compact))
    {
        run([&]
        {
            if (!FrameSelectedObject())
            {
                throw std::logic_error("Selected object has no frameable bounds in this viewport.");
            }
        });
    }
    context.SameLine();
    if (context.ToolbarButton("Duplicate##scene-tb-dup", ToolbarIcon::Duplicate, false, hasSelection, compact))
    {
        run([&] { DuplicateSelection(); });
    }
    context.SameLine();
    if (context.ToolbarButton("Delete##scene-tb-del", ToolbarIcon::Delete, false, hasSelection, compact))
    {
        run([&] { DeleteSelection(); });
    }
    separator();
    if (context.ToolbarButton("Undo##scene-tb-undo", ToolbarIcon::Undo, false, world.Commands().CanUndo(), compact))
    {
        run([&]
        {
            world.CommandRegistry().Invoke(
                editor_model::authoring_commands::kUndo);
        });
    }
    context.SameLine();
    if (context.ToolbarButton("Redo##scene-tb-redo", ToolbarIcon::Redo, false, world.Commands().CanRedo(), compact))
    {
        run([&]
        {
            world.CommandRegistry().Invoke(
                editor_model::authoring_commands::kRedo);
        });
    }

    if (world.Selection().Ordered().size() == 1U)
    {
        separator();
        DrawElementBubble(
            context,
            world.Selection().Ordered().front());
    }

    context.ToolbarDivider();
    if (context.ToolbarButton("Commands /##toolbar-commands", editor_ui::ToolbarIcon::More, false, true, true))
        expansion_.RequestCommandPaletteOpen();

    if (!status_.empty())
    {
        context.SameLine();
        context.MutedText(status_);
    }
}

void StudioViewportPanels::DrawPlanetToolbar(
    editor_ui::PanelContext& context)
{
    if (session_ == nullptr || !session_->World().HasWorld())
    {
        context.MutedText("Open a world to use the Planet tools.");
        return;
    }

    auto& world = session_->World();
    using editor_ui::ToolbarIcon;
    const bool compact = context.ContentAvailable().width <
        1900.0F * editor_ui::CurrentUiScale();

    const auto createPlanet = world.CommandRegistry().Enablement(
        editor_model::authoring_commands::kCreateRockyPlanet);
    if (context.ToolbarButton("Create Rocky Planet##planet-toolbar-create",
            ToolbarIcon::Planet, false, createPlanet.enabled, compact))
    {
        try
        {
            world.CommandRegistry().Invoke(editor_model::authoring_commands::kCreateRockyPlanet);
            status_.clear();
        }
        catch (const std::exception& exception) { status_ = exception.what(); }
    }
    context.ToolbarDivider();

    editor_model::CelestialAuthoringModel model(
        world.Objects(), world.Schemas(), world.Commands(), world.Selection());
    const auto body = model.SelectedBody();

    if (!body.has_value())
    {
        context.MutedText("Select a planet to edit its generation elements.");
    }
    else
    {
        const auto bodyChildren = world.Objects().Children(body->id);

        const auto toggleCapability = [&](const schema::TypeId type,
                                          const std::string_view label,
                                          const ToolbarIcon icon)
        {
            const auto state = model.CapabilityEnabled(body->id, type);
            bool enabled = state.value_or(false);
            if (context.ToolbarButton(std::string(label) + "##planet-feature-" +
                    type.ToString(), icon, enabled, true, compact))
            {
                try
                {
                    model.SetCapabilityEnabled(body->id, type, !enabled);
                    status_.clear();
                }
                catch (const std::exception& exception) { status_ = exception.what(); }
            }
            if (state.has_value())
            {
                for (const auto& child : world.Objects().Children(body->id))
                {
                    if (child.type == type)
                    {
                        context.SameLine();
                        DrawElementBubble(context, child.id);
                        break;
                    }
                }
            }
            context.SameLine();
        };

        const auto terrainSurface = std::ranges::find_if(
            bodyChildren,
            [](const auto& child)
            {
                return child.type == world_model::kTerrainSurfaceType;
            });
        editor_model::SurfaceAuthoringModel terrainAuthoring(
            world.Objects(), world.Commands(), world.Selection());
        if (terrainSurface != bodyChildren.end())
        {
            const scene::ObjectId terrainId = terrainSurface->id;
            context.SameLine();
            const bool openRivers = context.ToolbarButton(
                "Hydrology##planet-toolbar-rivers",
                ToolbarIcon::Procedural,
                false,
                true,
                compact,
                editor_ui::ToolbarStyle::Menu);
            context.AnchorNextPopupBelowItem();
            if (context.BeginPopup(
                    "planet-rivers-popup",
                    openRivers,
                    {520.0F * editor_ui::CurrentUiScale(), 0.0F}))
            {
                auto processes = terrainAuthoring.ProcessSettings(terrainId);
                context.Heading("Hydrology · Mountain-fed Rivers");
                context.MutedText("Regional drainage and connected river graphs. Channel size follows accumulated discharge.");
                context.Heading("Runoff budget");
                context.MutedText("Runoff uses this rainfall rate times the static terrain precipitation field. Seasonal forcing scales it with simulation time; groundwater is not modeled.");
                bool changed = context.InputDouble(
                    "Rainfall Rate (m/s)##planet-hydrology-rainfall",
                    processes.hydraulic.rainfallMetersPerSecond);
                const auto waterBalance = context.TreeItem("Infiltration and soil water##planet-hydrology-water-balance", false);
                if (waterBalance.open)
                {
                    changed |= context.InputDouble(
                        "Infiltration Rate (m/s)##planet-hydrology-infiltration",
                        processes.hydraulic.infiltrationMetersPerSecond);
                    changed |= context.InputDouble(
                        "Soil Moisture Capacity (m)##planet-hydrology-moisture-capacity",
                        processes.hydraulic.moistureCapacityDepthMeters);
                    changed |= context.InputDouble(
                        "Evaporation Rate (1/s)##planet-hydrology-evaporation",
                        processes.hydraulic.evaporationRatePerSecond);
                    context.TreePop();
                }
                const auto seasonal = context.TreeItem("Seasonal rainfall##planet-hydrology-seasonal", false);
                if (seasonal.open)
                {
                    context.MutedText(
                        "Simulation time modulates the static precipitation map; runoff is refreshed in 12 steps per season cycle.");
                    changed |= context.SliderDouble(
                        "Amplitude##planet-hydrology-seasonal-amplitude",
                        processes.hydraulic.seasonalRainfallAmplitude, 0.0, 1.0);
                    constexpr f64 secondsPerYear = 31'557'600.0;
                    f64 seasonYears =
                        processes.hydraulic.seasonalRainfallPeriodSeconds / secondsPerYear;
                    if (context.InputDouble(
                            "Season Length (years)##planet-hydrology-seasonal-period",
                            seasonYears))
                    {
                        processes.hydraulic.seasonalRainfallPeriodSeconds =
                            std::max(1.0 / secondsPerYear, seasonYears) * secondsPerYear;
                        changed = true;
                    }
                    f64 seasonPhaseDegrees =
                        processes.hydraulic.seasonalRainfallPhaseRadians * 180.0 / std::numbers::pi;
                    if (context.InputDouble(
                            "Phase Offset (degrees)##planet-hydrology-seasonal-phase",
                            seasonPhaseDegrees))
                    {
                        processes.hydraulic.seasonalRainfallPhaseRadians =
                            seasonPhaseDegrees * std::numbers::pi / 180.0;
                        changed = true;
                    }
                    context.TreePop();
                }
                context.Separator();
                changed |= context.Checkbox("Generate rivers##planet-rivers-enabled", processes.riversEnabled);
                if (processes.riversEnabled)
                {
                    context.MutedText("Straight reaches use this maximum spacing; headwaters, sharp bends, confluences and page exits are always retained.");
                    changed |= context.InputDouble(
                        "Maximum Graph Node Spacing (m)##planet-rivers-node-spacing",
                        processes.rivers.maximumNodeSpacingMeters);
                    changed |= context.InputDouble(
                        "Minimum Catchment (m²)##planet-rivers-area",
                        processes.rivers.minimumDrainageAreaSquareMeters);
                    changed |= context.InputDouble(
                        "Minimum Discharge (m³/s)##planet-rivers-discharge",
                        processes.rivers.minimumDischargeCubicMetersPerSecond);
                    changed |= context.Checkbox(
                        "Meander channels##planet-rivers-meanders",
                        processes.rivers.enableMeanders);
                    changed |= context.Checkbox(
                        "Allow cutoffs##planet-rivers-cutoffs",
                        processes.rivers.enableCutoffs);
                    const auto channel = context.TreeItem("Channel sizing · discharge to width/depth##planet-rivers-channel", false);
                    if (channel.open)
                    {
                        changed |= context.InputDouble("Reference Discharge (m³/s)##planet-rivers-reference-discharge", processes.rivers.referenceDischargeCubicMetersPerSecond);
                        changed |= context.InputDouble("Base Width (m)##planet-rivers-base-width", processes.rivers.baseChannelWidthMeters);
                        changed |= context.InputDouble("Minimum Width (m)##planet-rivers-min-width", processes.rivers.minimumChannelWidthMeters);
                        changed |= context.InputDouble("Maximum Width (m)##planet-rivers-max-width", processes.rivers.maximumChannelWidthMeters);
                        changed |= context.InputDouble("Width Scaling Exponent##planet-rivers-width-exponent", processes.rivers.widthDischargeExponent);
                        changed |= context.InputDouble("Base Depth (m)##planet-rivers-base-depth", processes.rivers.baseChannelDepthMeters);
                        changed |= context.InputDouble("Minimum Depth (m)##planet-rivers-min-depth", processes.rivers.minimumChannelDepthMeters);
                        changed |= context.InputDouble("Maximum Depth (m)##planet-rivers-max-depth", processes.rivers.maximumChannelDepthMeters);
                        changed |= context.InputDouble("Depth Scaling Exponent##planet-rivers-depth-exponent", processes.rivers.depthDischargeExponent);
                        context.TreePop();
                    }
                    if (processes.rivers.enableMeanders)
                    {
                        const auto meanders = context.TreeItem("Meander behavior##planet-rivers-meanders-advanced", false);
                        if (meanders.open)
                        {
                            i64 iterations = static_cast<i64>(processes.rivers.meanderIterations);
                            changed |= context.InputInteger("Iterations##planet-rivers-iterations", iterations);
                            processes.rivers.meanderIterations = iterations > 0 ? static_cast<u32>(iterations) : 1U;
                            changed |= context.InputDouble("Time Step##planet-rivers-meander-time", processes.rivers.meanderTimeStep);
                            changed |= context.InputDouble("Curvature Migration##planet-rivers-curvature-migration", processes.rivers.curvatureMigrationRate);
                            changed |= context.InputDouble("Seed Migration##planet-rivers-seed-migration", processes.rivers.deterministicSeedMigrationRate);
                            changed |= context.InputDouble("Maximum Offset (channel widths)##planet-rivers-offset", processes.rivers.maximumCenterlineOffsetWidths);
                            context.TreePop();
                        }
                    }
                    if (processes.rivers.enableCutoffs)
                    {
                        const auto cutoffs = context.TreeItem("Cutoff behavior##planet-rivers-cutoffs-advanced", false);
                        if (cutoffs.open)
                        {
                            i64 pathNodes = static_cast<i64>(processes.rivers.minimumCutoffPathNodes);
                            changed |= context.InputInteger("Minimum Path Nodes##planet-rivers-cutoff-nodes", pathNodes);
                            processes.rivers.minimumCutoffPathNodes = pathNodes >= 2 ? static_cast<u32>(pathNodes) : 2U;
                            changed |= context.InputDouble("Cutoff Distance (channel widths)##planet-rivers-cutoff-distance", processes.rivers.cutoffDistanceWidths);
                            context.TreePop();
                        }
                    }
                }
                if (changed)
                {
                    try
                    {
                        terrainAuthoring.SetProcessSettings(terrainId, processes);
                        const auto targetBody = session_->World().Surfaces().BodyForTerrainObject(terrainId);
                        if (!targetBody.has_value())
                            throw std::runtime_error("Terrain process settings require a spherical terrain body.");
                        const auto planet = session_->World().Surfaces().Registry().SphericalPlanetDefinition(*targetBody);
                        if (!planet.has_value())
                            throw std::runtime_error("Terrain process settings require a spherical planet definition.");
                        session_->QueueTerrainInvalidation({
                            .kind = terrain_dependency::TerrainChangeKind::ProcessSettings,
                            .scope = {.planet = planet->id, .global = true}});
                        status_ = "River recipe updated; drainage and channels are rebuilding.";
                    }
                    catch (const std::exception& exception) { status_ = exception.what(); }
                }
                if (views_ != nullptr)
                {
                    auto diagnostics = views_->TerrainDiagnosticOverlays("studio.primary");
                    if (context.Checkbox("Show drainage and river channels##planet-rivers-preview", diagnostics.drainageVectors))
                    {
                        views_->SetTerrainDiagnosticOverlays("studio.primary", diagnostics);
                        status_ = diagnostics.drainageVectors
                            ? "Showing downhill flow and connected river channels in the viewport."
                            : "River channel preview hidden.";
                    }
                }
                if (context.PrimaryButton("Draw Drainage Guidance Path##planet-hydrology-draw-path"))
                {
                    terrainTool_ = StudioTerrainAuthoringTool::DrainagePath;
                    terrainSplinePoints_.clear();
                    terrainSplineTerrain_.reset();
                    if (views_ != nullptr)
                        views_->ClearTerrainAuthoringOverlay("studio.primary");
                    status_ = "Click terrain to add drainage path points; double-click to commit.";
                }
                context.Separator();
                context.Heading("Planet map previews");
                context.MutedText("Map layers use the same terrain and climate samples as the viewport.");
                if (context.Button("Preview Rainfall Map##planet-hydrology-precipitation"))
                {
                    try
                    {
                        session_->Viewports().SetMode("studio.primary", studio_session::ViewportMode::FlatMap);
                        if (views_ != nullptr)
                            views_->SetFlatMapLayer("studio.primary", FlatMapLayer::Precipitation);
                        status_ = "Showing planet precipitation on the flat map.";
                    }
                    catch (const std::exception& exception) { status_ = exception.what(); }
                }
                if (context.Button("Preview Standing Water##planet-hydrology-water-depth"))
                {
                    try
                    {
                        session_->Viewports().SetMode("studio.primary", studio_session::ViewportMode::FlatMap);
                        if (views_ != nullptr)
                            views_->SetFlatMapLayer("studio.primary", FlatMapLayer::WaterDepth);
                        status_ = "Showing standing-water depth on the flat map.";
                    }
                    catch (const std::exception& exception) { status_ = exception.what(); }
                }
                context.Separator();
                context.Heading("Generated river nodes near viewport");
                const auto riverRuntime = session_->TerrainRuntime().Capture("studio.primary");
                if (!riverRuntime.has_value() || riverRuntime->terrainObject != terrainId)
                {
                    context.MutedText("Bind this planet to the primary viewport to inspect generated channels.");
                }
                else
                {
                    static f64 riverConstraintRadiusMeters = 500.0;
                    static f64 riverConstraintStrength = 1.0;
                    static_cast<void>(context.InputDouble(
                        "Constraint Radius (m)##planet-river-constraint-radius",
                        riverConstraintRadiusMeters));
                    static_cast<void>(context.SliderDouble(
                        "Constraint Strength##planet-river-constraint-strength",
                        riverConstraintStrength, 0.0, 1.0));
                    riverConstraintRadiusMeters = std::max(1.0, riverConstraintRadiusMeters);
                    const auto riverTiles = world::TileNeighborhood(
                        riverRuntime->observerPhysicalPage.tile, 1U);
                    u64 riverNodes = 0U;
                    u64 riverSegments = 0U;
                    u64 riverBoundaryLinks = 0U;
                    u64 pendingRiverPages = 0U;
                    u64 shownRiverNodes = 0U;
                    u64 shownRiverSegments = 0U;
                    u64 shownBoundaryLinks = 0U;
                    u64 lakeBasinCount = 0U;
                    u64 pendingLakePages = 0U;
                    u64 shownLakeBasins = 0U;
                    for (const auto& tile : riverTiles)
                    {
                        const terrain::PhysicalTerrainPageAddress address{
                            .planet = riverRuntime->planet.id,
                            .tile = tile};
                        const auto page = session_->TerrainPhysicalPages().Find(address);
                        if (page == nullptr || page->lakes == nullptr)
                        {
                            ++pendingLakePages;
                        }
                        else
                        {
                            lakeBasinCount += page->lakes->basins.size();
                            for (const auto& basin : page->lakes->basins)
                            {
                                if (shownLakeBasins >= 6U)
                                    break;
                                const u32 resolution = page->lakes->resolution;
                                const std::string spill = basin.spillCellIndex < resolution * resolution
                                    ? std::format("spill ({}, {}){}",
                                        basin.spillCellIndex % resolution,
                                        basin.spillCellIndex / resolution,
                                        basin.spillExitsPage ? " · exits page" : "")
                                    : "no routed spill cell";
                                const std::string downstream = basin.downstreamRiverNode.IsValid()
                                    ? std::format("M16 {} · {} cells downstream",
                                        basin.downstreamRiverNode.ToString(),
                                        basin.downstreamRiverCellCount)
                                    : basin.downstreamRiverExitsPage
                                        ? "overflow continues off page"
                                        : "no qualifying downstream channel";
                                context.MutedText(std::format(
                                    "Lake · {:.0f} m² · {:.1f} m deep · surface {:.1f} m · {} · {}",
                                    basin.areaSquareMeters, basin.maximumDepthMeters,
                                    basin.surfaceElevationMeters, spill, downstream));
                                ++shownLakeBasins;
                            }
                        }
                        if (page == nullptr || page->rivers == nullptr)
                        {
                            ++pendingRiverPages;
                            continue;
                        }
                        riverNodes += page->rivers->nodes.size();
                        riverSegments += page->rivers->segments.size();
                        riverBoundaryLinks += page->rivers->boundaryLinks.size();
                        for (const auto& link : page->rivers->boundaryLinks)
                        {
                            if (shownBoundaryLinks >= 6U)
                                break;
                            context.MutedText(std::format(
                                "Page continuation · basin {} · flow ({}, {}) · edge cell ({}, {})",
                                link.basin.ToString(), link.flowDx, link.flowDy,
                                link.targetX, link.targetY));
                            ++shownBoundaryLinks;
                        }
                        for (const auto& node : page->rivers->nodes)
                        {
                            if (shownRiverNodes >= 8U)
                                break;
                            const std::string nodeLabel = std::format(
                                "Node {} · {:.2f} m³/s · {:.1f} m wide · {:.2f} m/s##river-node-{}",
                                shownRiverNodes + 1U,
                                node.dischargeCubicMetersPerSecond,
                                node.channelWidthMeters,
                                node.velocityMetersPerSecond,
                                node.id.ToString());
                            if (context.Button(nodeLabel))
                            {
                                status_ = std::format(
                                    "River node {} · basin {} · discharge {:.3f} m³/s · width {:.2f} m · depth {:.2f} m · level {:.1f} m · slope {:.5f} · velocity {:.2f} m/s · roughness {:.3f} · section {:.1f} m² · suspended sediment {:.2f} kg.",
                                    node.id.ToString(), node.basin.ToString(),
                                    node.dischargeCubicMetersPerSecond,
                                    node.channelWidthMeters, node.channelDepthMeters,
                                    node.waterLevelMeters, node.slope,
                                    node.velocityMetersPerSecond, node.manningRoughness,
                                    node.crossSectionAreaSquareMeters, node.suspendedSedimentKg);
                            }
                            const terrain::PhysicalTerrainPageAddress nodePage{
                                .planet = riverRuntime->planet.id,
                                .tile = tile};
                            const math::Double2 nodeCenter{
                                node.channelOffsetMeters.x,
                                node.channelOffsetMeters.y};
                            const math::Double2 flowDirection{
                                static_cast<f64>(node.drainageFlowDx),
                                static_cast<f64>(node.drainageFlowDy)};
                            const math::Double2 constraintDirection =
                                std::hypot(flowDirection.x, flowDirection.y) > 1.0e-12
                                    ? flowDirection
                                    : math::Double2{1.0, 0.0};
                            const auto authorConstraint = [&](
                                const terrain_erosion::RiverConstraintKind kind,
                                const char* action)
                            {
                                try
                                {
                                    const auto constraint = terrainAuthoring.AddRiverBasinConstraint(
                                        terrainId, nodePage, node.basin, kind,
                                        nodeCenter, constraintDirection,
                                        riverConstraintRadiusMeters,
                                        riverConstraintStrength);
                                    terrainAuthoring.SelectObject(constraint);
                                    status_ = std::format(
                                        "{} constraint saved for basin {}.", action,
                                        node.basin.ToString());
                                }
                                catch (const std::exception& exception)
                                {
                                    status_ = exception.what();
                                }
                            };
                            if (context.Button(std::format(
                                    "Attract##river-attract-{}", node.id.ToString())))
                                authorConstraint(terrain_erosion::RiverConstraintKind::Attract, "Attract");
                            if (context.Button(std::format(
                                    "Repel##river-repel-{}", node.id.ToString())))
                                authorConstraint(terrain_erosion::RiverConstraintKind::Repel, "Repel");
                            if (context.Button(std::format(
                                    "Follow flow##river-trajectory-{}", node.id.ToString())))
                                authorConstraint(terrain_erosion::RiverConstraintKind::Trajectory, "Trajectory");
                            ++shownRiverNodes;
                        }
                        for (const auto& segment : page->rivers->segments)
                        {
                            if (shownRiverSegments >= 5U)
                                break;
                            context.MutedText(std::format(
                                "Reach · {} routing points · slope {:.5f} · {:.2f} m/s · {:.3f} roughness · {:.2f} kg suspended sediment",
                                segment.routingPathMeters.size(), segment.slope,
                                segment.velocityMetersPerSecond, segment.manningRoughness,
                                segment.suspendedSedimentKg));
                            ++shownRiverSegments;
                        }
                    }
                    context.MutedText(std::format(
                        "{} graph nodes · {} reaches · {} page continuations in {} nearby pages · {} pages still building.",
                        riverNodes, riverSegments, riverBoundaryLinks,
                        riverTiles.size() - pendingRiverPages, pendingRiverPages));
                    context.MutedText(std::format(
                        "{} M09 lake basins in {} nearby pages · {} lake pages still building.",
                        lakeBasinCount, riverTiles.size() - pendingLakePages, pendingLakePages));
                    if (lakeBasinCount > shownLakeBasins)
                        context.MutedText("Showing the first 6 basins; use orbit_terrain_lakes_nearby for full page-local spill details.");
                    if (riverBoundaryLinks > shownBoundaryLinks)
                        context.MutedText("Showing the first 6 page continuations; use orbit_terrain_rivers_nearby for the full boundary-link list.");
                    if (riverNodes > shownRiverNodes)
                        context.MutedText("Showing the first 8 nodes; use orbit_terrain_rivers_nearby for the full page result.");
                    if (riverSegments > shownRiverSegments)
                        context.MutedText("Showing the first 5 reaches; use orbit_terrain_rivers_nearby for the routing paths.");
                    if (riverNodes == 0U && pendingRiverPages == 0U)
                        context.MutedText("No river nodes meet the current drainage and discharge thresholds nearby.");
                }
                context.EndPopup();
            }
            context.SameLine();
            const bool openTectonics = context.ToolbarButton(
                "Tectonics##planet-toolbar-tectonics",
                ToolbarIcon::Procedural,
                false,
                true,
                compact,
                editor_ui::ToolbarStyle::Menu);
            context.AnchorNextPopupBelowItem();
            if (context.BeginPopup(
                    "planet-tectonics-popup",
                    openTectonics,
                    {520.0F * editor_ui::CurrentUiScale(), 0.0F}))
            {
                auto settings = terrainAuthoring.Tectonics(terrainId);
                context.Heading("Planetary Tectonics");
                context.MutedText("Recipe changes rebuild the same spherical plate field used by terrain and the tectonic map.");

                i32 preset = 0;
                static constexpr std::array<std::string_view, 3> kTectonicPresets{
                    "Earthlike", "Ancient / Stable", "Hotspot Rich"};
                if (context.Combo("Starting Recipe##planet-tectonics-preset", kTectonicPresets, preset))
                {
                    if (preset == 0)
                    {
                        settings = terrain::TectonicFieldDesc{};
                    }
                    else if (preset == 1)
                    {
                        settings.plateCount = 8U;
                        settings.continentalPlateFraction = 0.55;
                        settings.minPlateAngularSpeed = 0.08;
                        settings.maxPlateAngularSpeed = 0.38;
                        settings.hotspotCount = 2U;
                    }
                    else if (preset == 2)
                    {
                        settings.plateCount = 18U;
                        settings.continentalPlateFraction = 0.32;
                        settings.hotspotCount = 8U;
                        settings.hotspotBaseReliefMeters = 8'000.0;
                    }
                    try
                    {
                        terrainAuthoring.SetTectonics(terrainId, settings);
                        status_ = std::format("Applied {} tectonic recipe.", kTectonicPresets[static_cast<std::size_t>(preset)]);
                    }
                    catch (const std::exception& exception) { status_ = exception.what(); }
                }

                context.Heading("Plate Layout");
                i64 plateCount = static_cast<i64>(settings.plateCount);
                bool changed = context.InputInteger("Major Plates (1–24)##planet-tectonics-plates", plateCount);
                settings.plateCount = plateCount >= 0 ? static_cast<u32>(plateCount) : 0U;
                changed |= context.SliderDouble("Continental Fraction##planet-tectonics-continent-fraction", settings.continentalPlateFraction, 0.0, 1.0);
                changed |= context.SliderDouble("Plate Control of Continents##planet-tectonics-continent-influence", settings.tectonicContinentInfluence, 0.0, 1.0);
                context.Heading("Boundary Character");
                changed |= context.InputDouble("Convergent Uplift (m)##planet-tectonics-uplift", settings.convergenceUpliftMeters);
                changed |= context.SliderDouble("Oceanic Collision Relief##planet-tectonics-oceanic-scale", settings.oceanicConvergenceScale, 0.0, 2.0);
                changed |= context.SliderDouble("Belt Ridge Relief##planet-tectonics-belt-ridge-relief", settings.beltRidgeRelief, 0.0, 3.0);
                context.Heading("Hotspots");
                i64 hotspotCount = static_cast<i64>(settings.hotspotCount);
                changed |= context.InputInteger("Mantle Hotspots (0–8)##planet-tectonics-hotspots", hotspotCount);
                settings.hotspotCount = hotspotCount >= 0 ? static_cast<u32>(hotspotCount) : 0U;
                changed |= context.InputDouble("Hotspot Relief (m)##planet-tectonics-hotspot-relief", settings.hotspotBaseReliefMeters);

                const auto advanced = context.TreeItem("Advanced Plate Controls##planet-tectonics-advanced", false);
                if (advanced.open)
                {
                    i64 seed = static_cast<i64>(std::min<u64>(settings.seed, 0x7fffffffffffffffULL));
                    changed |= context.InputInteger("Tectonic Seed (0 = terrain seed)##planet-tectonics-seed", seed);
                    settings.seed = seed >= 0 ? static_cast<u64>(seed) : 0U;
                    changed |= context.SliderDouble("Plate Irregularity##planet-tectonics-irregularity", settings.plateIrregularity, 0.0, 1.0);
                    changed |= context.SliderDouble("Boundary Influence Width##planet-tectonics-boundary-width", settings.boundaryWidthDot, 0.01, 1.0);
                    changed |= context.InputDouble("Minimum Plate Motion##planet-tectonics-min-speed", settings.minPlateAngularSpeed);
                    changed |= context.InputDouble("Maximum Plate Motion##planet-tectonics-max-speed", settings.maxPlateAngularSpeed);
                    i64 ageSteps = static_cast<i64>(settings.hotspotAgeSteps);
                    changed |= context.InputInteger("Hotspot Chain Age Steps (0–6)##planet-tectonics-age-steps", ageSteps);
                    settings.hotspotAgeSteps = ageSteps >= 0 ? static_cast<u32>(ageSteps) : 0U;
                    changed |= context.SliderDouble("Hotspot Age Decay##planet-tectonics-hotspot-decay", settings.hotspotAgeDecay, 0.0, 1.0);
                    changed |= context.InputDouble("Hotspot Chain Spacing (m)##planet-tectonics-hotspot-spacing", settings.hotspotChainSpacingMeters);
                    changed |= context.InputDouble("Hotspot Core Radius (m)##planet-tectonics-hotspot-radius", settings.hotspotCoreRadiusMeters);
                    changed |= context.SliderDouble("Plate Size Variance##planet-tectonics-size-variance", settings.plateSizeVarianceDot, 0.0, 1.0);
                    changed |= context.InputDouble("Continental Crust Bias (m)##planet-tectonics-continental-bias", settings.continentalPlateBiasMeters);
                    changed |= context.InputDouble("Oceanic Crust Bias (m)##planet-tectonics-oceanic-bias", settings.oceanicPlateBiasMeters);
                    changed |= context.InputDouble("Convergence Reference Speed##planet-tectonics-convergence-speed", settings.convergenceReferenceSpeed);
                    changed |= context.InputDouble("Transform Reference Speed##planet-tectonics-transform-speed", settings.transformReferenceSpeed);
                    changed |= context.InputDouble("Hotspot Radius Growth per Age##planet-tectonics-hotspot-growth", settings.hotspotRadiusGrowthPerAge);
                    context.TreePop();
                }

                if (changed)
                {
                    try
                    {
                        terrainAuthoring.SetTectonics(terrainId, settings);
                        status_ = "Tectonic recipe updated; terrain generation is refreshing.";
                    }
                    catch (const std::exception& exception) { status_ = exception.what(); }
                }

                context.Separator();
                context.Heading("Geological Coupling");
                context.MutedText("How crust age and uplift shape erosion and watersheds. Edits regenerate terrain.");
                {
                    auto coupling = terrainAuthoring.ProcessSettings(terrainId);
                    bool couplingChanged = context.SliderDouble(
                        "Old Crust Erodes Faster##planet-tectonics-age-erodibility",
                        coupling.streamPower.ageErodibilityGain, 0.0, 4.0);
                    couplingChanged |= context.SliderDouble(
                        "Old Crust Loses Uplift##planet-tectonics-age-uplift",
                        coupling.streamPower.ageUpliftDecay, 0.0, 1.0);
                    couplingChanged |= context.SliderDouble(
                        "Belts Steer Watersheds##planet-tectonics-drainage-guidance",
                        coupling.streamPower.tectonicDrainageGuidance, 0.0, 1.0);
                    if (couplingChanged)
                    {
                        try
                        {
                            terrainAuthoring.SetProcessSettings(terrainId, coupling);
                            const auto targetBody = session_->World().Surfaces().BodyForTerrainObject(terrainId);
                            if (!targetBody.has_value())
                                throw std::runtime_error("Terrain process settings require a spherical terrain body.");
                            const auto planet = session_->World().Surfaces().Registry().SphericalPlanetDefinition(*targetBody);
                            if (!planet.has_value())
                                throw std::runtime_error("Terrain process settings require a spherical planet definition.");
                            session_->QueueTerrainInvalidation({
                                .kind = terrain_dependency::TerrainChangeKind::ProcessSettings,
                                .scope = {.planet = planet->id, .global = true}});
                            status_ = "Geological coupling updated; terrain generation is refreshing.";
                        }
                        catch (const std::exception& exception) { status_ = exception.what(); }
                    }
                }
                context.MutedText("Old crust erodes to lower, rounder relief; belts act as divides that steer routing toward basins. Routing never climbs uphill.");

                context.Separator();
                context.Heading("Planet Bake");
                context.MutedText("Terrain samples a baked planet structure and never evaluates plates while it generates. Editing the recipe rebakes in the background; the old bake keeps rendering until the new one is ready.");
                if (const auto bake = session_->TerrainBake().Status(terrainId); bake.has_value())
                {
                    std::string line = std::format(
                        "{} · {}x{} texels per face · {:.1f} MB",
                        terrain_bake::BakeStateName(bake->state),
                        bake->activeResolution, bake->activeResolution,
                        static_cast<f64>(bake->activeBytes) / (1024.0 * 1024.0));
                    if (bake->state == terrain_bake::BakeState::Baking)
                        line = std::format("baking {:.0f}% · {}", static_cast<f64>(bake->progress) * 100.0, line);
                    else if (bake->lastBakeSeconds > 0.0)
                        line += std::format(" · last bake {:.1f} s", bake->lastBakeSeconds);
                    context.MutedText(line);
                    context.MutedText(bake->riversActive
                        ? std::format("Rivers: {} reaches, {} nodes, {:.1f} MB", bake->riverSegments, bake->riverNodes,
                              static_cast<f64>(bake->riverBytes) / (1024.0 * 1024.0))
                        : std::string("Rivers: not baked yet"));
                    if (bake->state == terrain_bake::BakeState::Stale)
                        context.MutedText("The tectonics recipe changed since this bake; it is rebaking or waiting for Bake Now.");
                    if (!bake->error.empty())
                        context.MutedText(bake->error);

                    auto bakePolicy = terrainAuthoring.ProcessSettings(terrainId);
                    static constexpr std::array<u32, 4> kBakeResolutions{128U, 256U, 512U, 1024U};
                    static constexpr std::array<std::string_view, 4> kBakeResolutionLabels{
                        "128 (fast, ~80 km texels)", "256 (default, ~40 km)", "512 (~20 km)", "1024 (~10 km, 190 MB)"};
                    int resolutionIndex = 1;
                    for (std::size_t i = 0; i < kBakeResolutions.size(); ++i)
                        if (kBakeResolutions[i] <= bakePolicy.bake.resolution)
                            resolutionIndex = static_cast<int>(i);
                    bool bakePolicyChanged = context.Combo(
                        "Resolution##planet-bake-resolution", kBakeResolutionLabels, resolutionIndex);
                    if (bakePolicyChanged)
                        bakePolicy.bake.resolution = kBakeResolutions[static_cast<std::size_t>(resolutionIndex)];
                    bakePolicyChanged |= context.Checkbox(
                        "Rebake Automatically##planet-bake-auto", bakePolicy.bake.autoRebake);
                    if (bakePolicyChanged)
                    {
                        try
                        {
                            terrainAuthoring.SetProcessSettings(terrainId, bakePolicy);
                            status_ = "Planet bake policy updated.";
                        }
                        catch (const std::exception& exception) { status_ = exception.what(); }
                    }

                    if (context.PrimaryButton("Bake Now##planet-bake-start"))
                    {
                        status_ = session_->TerrainBake().StartBake(terrainId)
                            ? "Planet bake started; terrain keeps using the current bake until it finishes."
                            : "The planet bake could not be started.";
                    }
                    if (bake->state == terrain_bake::BakeState::Baking)
                    {
                        context.SameLine();
                        if (context.Button("Cancel##planet-bake-cancel"))
                        {
                            session_->TerrainBake().Cancel(terrainId);
                            status_ = "Planet bake cancelled; the active bake is untouched.";
                        }
                    }
                }
                else
                {
                    context.MutedText("No bake yet: the planet bakes when its terrain is composed.");
                }
                context.MutedText("Same operations as terrain.bake_status / bake_start / bake_cancel / bake_set.");

                context.Separator();
                context.Heading("Structural Sample");
                static_cast<void>(context.Checkbox(
                    "Under Observer##planet-tectonics-probe-observer", tectonicProbeAtObserver_));
                if (!tectonicProbeAtObserver_)
                {
                    static_cast<void>(context.InputDouble(
                        "Latitude (deg)##planet-tectonics-probe-lat", tectonicProbeLatitude_));
                    static_cast<void>(context.InputDouble(
                        "Longitude (deg)##planet-tectonics-probe-lon", tectonicProbeLongitude_));
                    tectonicProbeLatitude_ = std::clamp(tectonicProbeLatitude_, -90.0, 90.0);
                }
                std::optional<math::Double3> probeDirection;
                if (!tectonicProbeAtObserver_)
                    probeDirection = studio_session::TectonicsDirectionFromLatLon(
                        tectonicProbeLatitude_, tectonicProbeLongitude_);
                const auto probe = studio_session::ProbeTectonicStructure(
                    *session_, "studio.primary", probeDirection);
                if (probe.has_value())
                {
                    const auto& s = probe->structure;
                    context.MutedText(std::format(
                        "{:.2f}°, {:.2f}° · plate {} ({}) beside {} ({}) · {} boundary {:.0f}%",
                        probe->latitudeDegrees, probe->longitudeDegrees,
                        s.plateId, s.continental ? "continental" : "oceanic",
                        s.neighbourPlateId, s.neighbourContinental ? "continental" : "oceanic",
                        terrain::TectonicBoundaryTypeName(s.boundaryType), s.boundaryStrength * 100.0));
                    context.MutedText(std::format(
                        "Crust {:.1f} km thick · age {:.2f} · geological age {:.2f}",
                        s.crustThicknessKm, s.crustAge, s.geologicalAge));
                    context.MutedText(std::format(
                        "Uplift {:.0f} m · subsidence {:.0f} m · stress {:.0f}% · volcanism {:.0f}%",
                        s.upliftMeters, s.subsidenceMeters, s.stress * 100.0, s.volcanism * 100.0));
                }
                else
                {
                    context.MutedText("No analytic terrain runtime is available to sample.");
                }
                context.MutedText("Same data as orbit_terrain_tectonics_sample.");

                context.Separator();
                if (context.PrimaryButton("Preview Tectonic Plates##planet-tectonics-preview"))
                {
                    try
                    {
                        session_->Viewports().SetMode("studio.primary", studio_session::ViewportMode::FlatMap);
                        if (views_ != nullptr)
                            views_->SetFlatMapLayer("studio.primary", FlatMapLayer::Tectonics);
                        status_ = "Showing plate identities and convergent / divergent / transform boundary influence.";
                    }
                    catch (const std::exception& exception) { status_ = exception.what(); }
                }
                context.MutedText("Map colors identify plates; red = convergent, cyan = divergent, yellow = transform influence.");
                context.EndPopup();
            }
        }

        context.ToolbarDivider();

        toggleCapability(world_model::kSurfaceCapabilityType, "Surface", ToolbarIcon::Surface);
        toggleCapability(world_model::kAtmosphereCapabilityType, "Atmosphere", ToolbarIcon::Atmosphere);
        toggleCapability(world_model::kCloudLayerCapabilityType, "Clouds / Volumetrics", ToolbarIcon::Clouds);
        toggleCapability(world_model::kOceanCapabilityType, "Ocean", ToolbarIcon::Ocean);
        toggleCapability(world_model::kRingSystemCapabilityType, "Rings", ToolbarIcon::Rings);
        toggleCapability(world_model::kMagnetosphereCapabilityType, "Aurora", ToolbarIcon::Aurora);

        const auto atmosphere = std::ranges::find_if(
            bodyChildren,
            [](const auto& child) { return child.type == world_model::kAtmosphereCapabilityType; });
        const bool hasAtmosphere = atmosphere != bodyChildren.end();
        const bool atmosphereMenu = context.ToolbarButton(
            "Atmosphere Setup##planet-atmosphere-setup", ToolbarIcon::Atmosphere,
            false, hasAtmosphere, compact, editor_ui::ToolbarStyle::Menu);
        context.AnchorNextPopupBelowItem();
        if (context.BeginPopup("planet-atmosphere-setup-popup", atmosphereMenu,
                {320.0F * editor_ui::CurrentUiScale(), 0.0F}))
        {
            context.Heading("Atmosphere Generation");
            if (hasAtmosphere)
            {
                world_model::AtmospherePropertySolver solver(world.Objects(), world.Commands());
                for (const auto preset : world_model::AtmospherePropertySolver::Presets())
                {
                    if (context.Button(std::string(preset) + "##planet-atmosphere-preset-" + std::string(preset)))
                    {
                        try
                        {
                            const auto report = solver.ApplyPreset(atmosphere->id, preset);
                            status_ = std::format("Applied {} atmosphere preset; {} values derived.",
                                preset, report.DerivedCount());
                        }
                        catch (const std::exception& exception) { status_ = exception.what(); }
                    }
                }
                if (context.PrimaryButton("Solve Derived Coefficients##planet-atmosphere-solve"))
                {
                    try
                    {
                        const auto report = solver.Solve(atmosphere->id);
                        status_ = std::format("Atmosphere solve derived {} values{}.",
                            report.DerivedCount(), report.HasConflict() ? " with conflicts" : "");
                    }
                    catch (const std::exception& exception) { status_ = exception.what(); }
                }
                context.MutedText("Use Properties for exact atmospheric coefficients and authoring modes.");
            }
            context.EndPopup();
        }
        context.SameLine();

        const auto capabilities = model.AvailableCapabilities();
        const bool openMore = context.ToolbarButton(
            "More Planet Elements##planet-toolbar-more", ToolbarIcon::More,
            false, true, compact, editor_ui::ToolbarStyle::Menu);
        context.AnchorNextPopupBelowItem();
        if (context.BeginPopup("planet-toolbar-more-popup", openMore,
                {360.0F * editor_ui::CurrentUiScale(), 0.0F}))
        {
            context.Heading("Additional Body Elements");
            for (const auto& capability : capabilities)
            {
                const auto type = capability.type;
                if (type == world_model::kSurfaceCapabilityType ||
                    type == world_model::kAtmosphereCapabilityType ||
                    type == world_model::kCloudLayerCapabilityType ||
                    type == world_model::kOceanCapabilityType ||
                    type == world_model::kRingSystemCapabilityType ||
                    type == world_model::kMagnetosphereCapabilityType)
                    continue;
                bool enabled = model.CapabilityEnabled(body->id, type).value_or(false);
                if (context.Checkbox(std::string(capability.label) + "##planet-extra-" + type.ToString(), enabled))
                {
                    try { model.SetCapabilityEnabled(body->id, type, enabled); status_.clear(); }
                    catch (const std::exception& exception) { status_ = exception.what(); }
                }
            }
            context.EndPopup();
        }
    }

    if (!status_.empty())
    {
        context.SameLine();
        context.MutedText(status_);
    }
}

void StudioViewportPanels::DrawWorkspaceBand(
    editor_ui::PanelContext& context)
{
    if (g_workspaceUi == nullptr)
    {
        return;
    }

    using editor_ui::NavigationIcon;
    static constexpr std::array<editor_ui::NavigationTab, 7> kWorkspaceModes{{
        {"Build", NavigationIcon::Build},
        {"Planet", NavigationIcon::Planet},
        {"Universe", NavigationIcon::Universe},
        {"Simulation", NavigationIcon::Simulation},
        {"Shading", NavigationIcon::Shading},
        {"Planning", NavigationIcon::Planning},
        {"Plugins", NavigationIcon::Plugins}
    }};

    i32 workspace = static_cast<i32>(g_workspaceMode);
    if (context.NavigationTabs(
            "studio-workspace-tabs",
            kWorkspaceModes,
            workspace))
    {
        workspace = std::clamp(workspace, 0, 6);
        ActivateWorkspace(
            *g_workspaceUi,
            static_cast<WorkspaceMode>(workspace));
    }
}

i32 StudioViewportPanels::QuickCreateCommandPriority(
    const std::string_view category) const noexcept
{
    const auto matches =
        [category](const std::string_view value) noexcept
        {
            return category.find(value) != std::string_view::npos;
        };

    switch (g_workspaceMode)
    {
    case WorkspaceMode::Scene:
        return matches("Scene") ||
               matches("World") ||
               matches("Path")
            ? 3
            : 1;

    case WorkspaceMode::Planet:
        return matches("Planet") ||
               matches("Terrain") ||
               matches("Biome") ||
               matches("World")
            ? 4
            : 1;

    case WorkspaceMode::Celestial:
        return matches("Celestial") ||
               matches("World")
            ? 4
            : 1;

    case WorkspaceMode::Simulation:
        if (matches("Simulation") ||
            matches("Volume") ||
            matches("Physics"))
        {
            return 4;
        }
        return matches("World") ||
               matches("Scene")
            ? 2
            : 1;

    case WorkspaceMode::Shading:
        return matches("Shading") ||
               matches("Material")
            ? 4
            : 0;

    case WorkspaceMode::Planning:
    case WorkspaceMode::Plugins:
        return 0;
    }

    return 1;
}

bool StudioViewportPanels::PreferCommandQuickCreate() const noexcept
{
    return g_workspaceMode == WorkspaceMode::Planet ||
        g_workspaceMode == WorkspaceMode::Celestial ||
        g_workspaceMode == WorkspaceMode::Shading;
}

bool StudioViewportPanels::ShowViewportQuickCreate() const noexcept
{
    return UsesCanonicalViewportWorkspace(g_workspaceMode);
}

void StudioViewportPanels::DrawContextBand(
    editor_ui::PanelContext& context)
{
    context.Text("Context");
    context.SameLine();

    if (session_ == nullptr ||
        !session_->World().HasWorld())
    {
        context.MutedText(
            "Open a world to expose contextual tools.");
        return;
    }

    auto& world = session_->World();
    const auto& selection =
        world.Selection().Ordered();
    auto* renderView =
        views_ != nullptr
            ? views_->Find("studio.primary")
            : nullptr;

    const auto invokeAuthoringCommand =
        [this](const commands::CommandId command)
        {
            try
            {
                session_->World().CommandRegistry().Invoke(command);
                status_.clear();
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
            }
        };

    std::optional<scene::ObjectRecord> selectedRecord;
    if (selection.size() == 1U)
    {
        selectedRecord =
            world.Objects().Find(selection.front());
    }

    const bool lightRelevant =
        selectedRecord.has_value() &&
        (selectedRecord->type == world_model::kPointLightType ||
         selectedRecord->type == world_model::kSpotLightType);

    const bool pathPairRelevant =
        world.CommandRegistry().Enablement(
            editor_model::authoring_commands::kConnectPathDirect).
            enabled;

    VolumeAuthoringUi* const volumeAuthoring =
        VolumeAuthoringUi::ContextInstance();
    const bool volumeRelevant =
        volumeAuthoring != nullptr &&
        volumeAuthoring->RelevantToSelection();

    if (lightRelevant && renderView != nullptr)
    {
        const scene::ObjectId lightId = selectedRecord->id;
        const bool spot =
            selectedRecord->type == world_model::kSpotLightType;

        f64 intensity = 1'000.0;
        if (const auto value = world.Objects().GetProperty(
                lightId,
                world_model::kLightIntensityLumens);
            value.has_value())
        {
            if (const auto* stored = std::get_if<f64>(&*value);
                stored != nullptr)
            {
                intensity = *stored;
            }
        }

        bool enabled = true;
        if (const auto value = world.Objects().GetProperty(
                lightId,
                world_model::kLightEnabled);
            value.has_value())
        {
            if (const auto* stored = std::get_if<bool>(&*value);
                stored != nullptr)
            {
                enabled = *stored;
            }
        }

        context.Text(
            std::format(
                "{} - {:.0f} lm",
                spot ? "Spot Light" : "Point Light",
                intensity));
        context.SameLine();

        const auto mutateLight =
            [this, lightId](
                const std::string_view name,
                const auto& mutation)
            {
                auto& commands = session_->World().Commands();
                commands.BeginTransaction(std::string(name));

                try
                {
                    mutation(commands);
                    commands.CommitTransaction();
                    status_.clear();
                }
                catch (...)
                {
                    if (commands.HasActiveTransaction())
                    {
                        commands.RollbackTransaction();
                    }
                    throw;
                }
            };

        if (context.Button(
                "Move To View##quick-light-move"))
        {
            try
            {
                const auto& camera = renderView->Camera();
                const math::Double3 position{
                    camera.localPositionMeters.x +
                        static_cast<f64>(camera.forward.x) * 5.0,
                    camera.localPositionMeters.y +
                        static_cast<f64>(camera.forward.y) * 5.0,
                    camera.localPositionMeters.z +
                        static_cast<f64>(camera.forward.z) * 5.0
                };

                mutateLight(
                    "Move Light To View",
                    [&](commands::CommandService& commands)
                    {
                        commands.SetProperty(
                            lightId,
                            world_model::kLightPositionMeters,
                            position);
                    });
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
            }
        }

        if (spot)
        {
            context.SameLine();
            if (context.Button(
                    "Aim Along View##quick-light-aim"))
            {
                try
                {
                    const auto forward = renderView->Camera().forward;
                    mutateLight(
                        "Aim Spot Light Along View",
                        [&](commands::CommandService& commands)
                        {
                            commands.SetProperty(
                                lightId,
                                world_model::kLightDirection,
                                math::Double3{
                                    static_cast<f64>(forward.x),
                                    static_cast<f64>(forward.y),
                                    static_cast<f64>(forward.z)
                                });
                        });
                }
                catch (const std::exception& exception)
                {
                    status_ = exception.what();
                }
            }
        }

        context.SameLine();
        if (context.Button(
                "Intensity -##quick-light-intensity-down"))
        {
            try
            {
                const f64 next = std::max(0.0, intensity / 1.25);
                mutateLight(
                    "Reduce Light Intensity",
                    [&](commands::CommandService& commands)
                    {
                        commands.SetProperty(
                            lightId,
                            world_model::kLightIntensityLumens,
                            next);
                    });
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
            }
        }

        context.SameLine();
        if (context.Button(
                "Intensity +##quick-light-intensity-up"))
        {
            try
            {
                const f64 next =
                    intensity <= 0.0
                        ? 100.0
                        : intensity * 1.25;
                mutateLight(
                    "Increase Light Intensity",
                    [&](commands::CommandService& commands)
                    {
                        commands.SetProperty(
                            lightId,
                            world_model::kLightIntensityLumens,
                            next);
                    });
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
            }
        }

        context.SameLine();
        if (context.Button(
                enabled
                    ? "Disable##quick-light-enabled"
                    : "Enable##quick-light-enabled"))
        {
            try
            {
                mutateLight(
                    enabled
                        ? "Disable Light"
                        : "Enable Light",
                    [&](commands::CommandService& commands)
                    {
                        commands.SetProperty(
                            lightId,
                            world_model::kLightEnabled,
                            !enabled);
                    });
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
            }
        }
        return;
    }

    if (pathPairRelevant)
    {
        context.Text("2 Path Nodes - Connect");
        context.SameLine();

        if (context.Button("Direct##quick-path-direct"))
        {
            invokeAuthoringCommand(
                editor_model::authoring_commands::kConnectPathDirect);
        }
        context.SameLine();
        if (context.Button("Bezier##quick-path-bezier"))
        {
            invokeAuthoringCommand(
                editor_model::authoring_commands::kConnectPathBezier);
        }
        context.SameLine();
        if (context.Button("Routed##quick-path-routed"))
        {
            invokeAuthoringCommand(
                editor_model::authoring_commands::kConnectPathRouted);
        }
        return;
    }

    if (volumeRelevant)
    {
        context.Text("Volume");
        context.SameLine();

        const auto sourceEnablement =
            world.CommandRegistry().Enablement(
                editor_model::authoring_commands::kAddVolumeSource);
        const auto effectorEnablement =
            world.CommandRegistry().Enablement(
                editor_model::authoring_commands::kAddVolumeEffector);

        bool drewButton = false;
        if (sourceEnablement.enabled)
        {
            if (context.Button(
                    "Add Source##quick-volume-source"))
            {
                invokeAuthoringCommand(
                    editor_model::authoring_commands::kAddVolumeSource);
            }
            drewButton = true;
        }

        if (effectorEnablement.enabled)
        {
            if (drewButton)
            {
                context.SameLine();
            }
            if (context.Button(
                    "Add Effector##quick-volume-effector"))
            {
                invokeAuthoringCommand(
                    editor_model::authoring_commands::kAddVolumeEffector);
            }
            drewButton = true;
        }

        if (drewButton)
        {
            context.SameLine();
        }
        if (context.Button(
                "Expert##quick-volume-expert") &&
            g_workspaceUi != nullptr)
        {
            static_cast<void>(
                g_workspaceUi->FocusPanelByTitle("Volumes"));
        }
        return;
    }

    switch (g_workspaceMode)
    {
    case WorkspaceMode::Scene:
        context.MutedText(
            "Select an object, or select two path nodes in the same network to connect them.");
        break;
    case WorkspaceMode::Planet:
        context.MutedText(
            "Select a rocky body or terrain surface to expose terrain authoring tools.");
        break;
    case WorkspaceMode::Simulation:
        context.Text("Simulation");
        context.SameLine();
        if (g_workspaceUi != nullptr &&
            context.Button(
                "Open Volume Expert##quick-volume-fallback"))
        {
            static_cast<void>(
                g_workspaceUi->FocusPanelByTitle("Volumes"));
        }
        break;
    case WorkspaceMode::Celestial:
    case WorkspaceMode::Shading:
        context.MutedText(
            std::format(
                "{} tools are available contextually in Properties.",
                WorkspaceName(g_workspaceMode)));
        break;
    case WorkspaceMode::Planning:
    case WorkspaceMode::Plugins:
        context.MutedText(
            "Use the active workspace panel for planning or plugin management.");
        break;
    }
}

void StudioViewportPanels::DrawActivityBand(
    editor_ui::PanelContext& context)
{
    if (g_workspaceUi == nullptr)
    {
        return;
    }

    using StatusClock = std::chrono::steady_clock;
    static StatusClock::time_point previousFrame{};
    static f64 smoothedFrameSeconds = 0.0;

    const auto now = StatusClock::now();
    if (previousFrame != StatusClock::time_point{})
    {
        const f64 elapsed =
            std::chrono::duration<f64>(
                now - previousFrame).count();

        if (elapsed > 0.0 && elapsed < 1.0)
        {
            constexpr f64 kSmoothing = 0.10;
            smoothedFrameSeconds =
                smoothedFrameSeconds <= 0.0
                    ? elapsed
                    : smoothedFrameSeconds +
                        (elapsed - smoothedFrameSeconds) *
                            kSmoothing;
        }
    }
    previousFrame = now;

    const bool hasWorld =
        session_ != nullptr &&
        session_->World().HasWorld();
    const std::string_view state =
        !status_.empty()
            ? std::string_view{"Attention"}
            : hasWorld
                ? std::string_view{"Ready"}
                : std::string_view{"No world"};

    context.Text(state);
    context.SameLine();
    context.MutedText("|");
    context.SameLine();
    context.Text("Vulkan");

    if (smoothedFrameSeconds > 0.0)
    {
        context.SameLine();
        context.MutedText("|");
        context.SameLine();
        context.Text(
            std::format(
                "{:.0f} FPS",
                1.0 / smoothedFrameSeconds));
    }

    if (ecoModeGetter_ && ecoModeSetter_)
    {
        context.SameLine();
        context.MutedText("|");
        context.SameLine();
        const bool enabled = ecoModeGetter_();
        const editor_ui::ActionPresentation ecoAction{
            .label = enabled ? "[Eco]" : "Eco",
            .enabled = true,
            .invoke = [this, enabled]
            {
                if (ecoModeSetter_)
                {
                    ecoModeSetter_(!enabled);
                }
            }};
        context.Toolbar(std::span{&ecoAction, 1});
    }

    const auto panels = g_workspaceUi->Panels();
    std::vector<editor_ui::ActionPresentation> actions;

    const auto appendPanel =
        [&panels, &actions](
            const std::string_view title,
            const std::string_view display)
        {
            const auto panel =
                std::ranges::find_if(
                    panels,
                    [title](
                        const editor_ui::EditorUi::PanelSummary& candidate)
                    {
                        return candidate.title == title &&
                            candidate.region == editor_ui::DockRegion::Bottom;
                    });

            if (panel == panels.end())
            {
                return;
            }

            const bool visible = panel->visible;
            std::string label =
                visible
                    ? std::string{"["} +
                          std::string(display) + "]"
                    : std::string(display);
            label += "##activity-";
            label += std::string(title);

            actions.push_back({
                .label = std::move(label),
                .enabled = true,
                .invoke =
                    [title, visible]
                    {
                        if (g_workspaceUi == nullptr)
                        {
                            return;
                        }

                        if (visible)
                        {
                            static_cast<void>(
                                g_workspaceUi->ClosePanelByTitle(title));
                        }
                        else
                        {
                            static_cast<void>(
                                g_workspaceUi->FocusPanelByTitle(title));
                        }
                    }
            });
        };

    appendPanel("Output", "Console");
    appendPanel("Build", "Build");
    appendPanel("Tasks", "Tasks");
    appendPanel("Display Diagnostics", "Diagnostics");

    if (!actions.empty())
    {
        context.SameLine();
        context.MutedText("|");
        context.SameLine();
        context.Toolbar(actions);
    }

    if (!status_.empty())
    {
        context.SameLine();
        context.MutedText("|");
        context.SameLine();
        context.MutedText(status_);
    }
}

void StudioViewportPanels::RegisterContextInspector(
    editor_ui::EditorUi& ui)
{
    if (ui.HasPanel(kContextInspectorPanel))
    {
        return;
    }

    ui.RegisterPanel({
        .id = kContextInspectorPanel,
        .title = "Inspector",
        .defaultOpen = false,
        .defaultDock = editor_ui::DockRegion::Right,
        .dockOrder = -100,
        .minSize = {
            .width = 300.0F,
            .height = 300.0F
        },
        .defaultSize = {
            .width = 390.0F,
            .height = 820.0F
        },
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawContextInspector(context);
            }
    });
}

void StudioViewportPanels::DrawContextInspector(
    editor_ui::PanelContext& context)
{
    if (session_ == nullptr ||
        !session_->World().HasWorld())
    {
        context.Text(
            "Open a world to inspect authored objects.");
        return;
    }

    auto& world = session_->World();
    auto& inspector = world.Inspector();
    const auto selected = inspector.SelectedObjects();

    context.Heading("Inspector");
    context.MutedText(
        "Expert compatibility view - canonical editing lives in Properties.");

    if (selected.empty())
    {
        context.Text("No selection");
        context.MutedText(
            "Select an object in World or the active workspace. Relevant authoring controls appear here automatically.");
        return;
    }

    if (selected.size() == 1U)
    {
        const auto& record = selected.front();
        context.Text(record.name);
        context.MutedText(record.type.ToString());
    }
    else
    {
        context.Text(
            std::format(
                "{} objects selected",
                selected.size()));
    }

    if (context.Section(
            "Properties##context-inspector-properties",
            true))
    {
        static_cast<void>(
            context.Checkbox(
                "Advanced##context-inspector-advanced",
                contextualAdvancedProperties_));

        if (!contextualAdvancedProperties_)
        {
            context.MutedText(
                "Advanced schema fields stay available here without becoming the default editing surface.");
        }

        context.Separator();

        for (auto property : inspector.CommonProperties())
        {
            if (property.schema.advanced &&
                !contextualAdvancedProperties_)
            {
                continue;
            }

            context.Text(
                std::format(
                    "{}{}{}",
                    property.schema.name,
                    property.schema.unit.empty()
                        ? ""
                        : " [",
                    property.schema.unit.empty()
                        ? ""
                        : property.schema.unit + "]"));

            if (property.mixed)
            {
                context.MutedText("<mixed>");
            }

            if (property.schema.readOnly)
            {
                context.MutedText("<read only>");
                context.Separator();
                continue;
            }

            const std::string label =
                "##context-property-" +
                property.schema.id.ToString();

            bool changed = false;

            std::visit(
                [&](auto& value)
                {
                    using Value =
                        std::decay_t<decltype(value)>;

                    if constexpr (std::is_same_v<Value, bool>)
                    {
                        changed = context.Checkbox(label, value);
                    }
                    else if constexpr (std::is_same_v<Value, i64>)
                    {
                        changed = context.InputInteger(label, value);
                    }
                    else if constexpr (std::is_same_v<Value, f64>)
                    {
                        changed = context.InputDouble(label, value);
                    }
                    else if constexpr (std::is_same_v<Value, std::string>)
                    {
                        changed = context.InputText(label, value);
                    }
                    else if constexpr (std::is_same_v<Value, math::Double3>)
                    {
                        changed = context.InputDouble3(label, value);
                    }
                    else if constexpr (requires { value.high; value.low; })
                    {
                        const scene::ObjectId id{
                            .high = value.high,
                            .low = value.low
                        };

                        context.MutedText(
                            id.IsValid()
                                ? id.ToString()
                                : "<none>");
                    }
                    else
                    {
                        context.MutedText(
                            "<unsupported property editor>");
                    }
                },
                property.value);

            if (changed)
            {
                try
                {
                    inspector.SetForSelection(
                        property.schema.id,
                        property.value);
                }
                catch (const std::exception& exception)
                {
                    status_ = exception.what();
                }
            }

            context.Separator();
        }
    }

    const auto providers =
        GlobalInspectorProviders().Relevant();

    if (!providers.empty())
    {
        context.Separator();
        context.MutedText("Contextual Tools");
        static_cast<void>(
            GlobalInspectorProviders().DrawRelevant(context));
    }
    else
    {
        context.MutedText(
            "No specialized authoring section is needed for this selection. Common schema properties remain fully editable above.");
    }
}

void StudioViewportPanels::Register(
    editor_ui::EditorUi& ui)
{
    if (views_ != nullptr)
    {
        views_->CreateDefaults();
    }

    ui.RegisterPanel({
        .id = kPrimaryViewportPanel,
        .title = "Viewport",
        .defaultOpen = true,
        .defaultDock = editor_ui::DockRegion::Center,
        .dockOrder = 0,
        .minSize = {
            .width = 320.0F,
            .height = 200.0F
        },
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawView(context, "studio.primary");
            }
    });

    ui.RegisterPanel({
        .id = kSecondaryViewportPanel,
        .title = "Body Map / Debug View",
        .defaultOpen = false,
        .defaultDock = editor_ui::DockRegion::Center,
        .dockOrder = 10,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawView(context, "studio.map");
            }
    });

    RegisterWorldAssetsBrowser(ui);
    RegisterContextInspector(ui);
    RegisterWorkspaceActions(ui);
    RegisterShellBands(ui);
}

void StudioViewportPanels::RegisterSecondary(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kSecondaryViewportPanel,
        .title = "Body Map / Debug View",
        .defaultOpen = false,
        .defaultDock = editor_ui::DockRegion::Center,
        .dockOrder = 10,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawView(context, "studio.map");
            }
    });

    RegisterWorldAssetsBrowser(ui);
    RegisterContextInspector(ui);
    RegisterWorkspaceActions(ui);
    RegisterShellBands(ui);
}

void StudioViewportPanels::DrawView(
    editor_ui::PanelContext& context,
    const std::string_view id)
{
    DrawViewBase(context, id);
}

bool StudioViewportPanels::HandleViewportGizmo(
    editor_ui::PanelContext& context,
    const std::string_view id)
{
    if (views_ == nullptr || session_ == nullptr)
    {
        return false;
    }

    // Handles only make sense in a perspective view of a body; the map and
    // debug views have no 3D camera to project them with.
    const auto* const target = session_->Viewports().Find(id);

    if (target == nullptr ||
        target->mode != studio_session::ViewportMode::Perspective)
    {
        manipulatorUi_.Cancel();
        return false;
    }

    return manipulatorUi_.Handle(
        context,
        *views_,
        *session_,
        id,
        expansion_.ViewportState().gizmo,
        gizmoRelativeMouseDelta_);
}

bool StudioViewportPanels::GizmoDragging() const noexcept
{
    return manipulatorUi_.Dragging();
}
} // namespace orbit::studio_ui
