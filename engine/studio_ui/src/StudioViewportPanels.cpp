#include <orbit/studio_ui/StudioViewportPanels.hpp>
#include <orbit/editor_model/CelestialAuthoringModel.hpp>

#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/studio_ui/StudioShellModel.hpp>
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>
#include <orbit/world_model/VisibilityProxyBinding.hpp>
#include <orbit/world_model/LocalLightBinding.hpp>

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
        tool == StudioTerrainAuthoringTool::Ridge;
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
                        terrainTool_ ==
                                StudioTerrainAuthoringTool::Canyon
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
                    status_ =
                        "Terrain spline committed with bounded M27 invalidation.";
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
    const u32 width =
        static_cast<u32>(
            std::max(available.width, 1.0F));
    const u32 height =
        static_cast<u32>(
            std::max(available.height, 1.0F));

    if (renderView->Width() != width ||
        renderView->Height() != height)
    {
        views_->Resize(id, width, height);
        renderView = views_->Find(id);
    }

    const auto imageInteraction =
        context.Image(
            renderView->DisplayColor(),
            {
                .width = static_cast<f32>(
                    renderView->Width()),
                .height = static_cast<f32>(
                    renderView->Height())
            });

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
                                terrainTool_ ==
                                        StudioTerrainAuthoringTool::
                                            Canyon
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
                            status_ =
                                "Terrain spline committed with bounded M27 invalidation.";
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
            "Explorer",
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
            "Project Settings"
        });
}

void OpenCanonicalBrowser(
    editor_ui::EditorUi& ui,
    const BrowserMode mode)
{
    g_browserMode = mode;
    CloseLegacyBrowserSources(ui);
    OpenPanels(ui, {"World / Assets"});
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
        .label = "Workspace: Scene",
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
        .label = "Workspace: Celestial",
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
        .title = "World / Assets",
        .defaultOpen = contract.defaultOpen,
        .defaultDock = contract.defaultDock,
        .dockOrder = contract.dockOrder,
        .minSize = contract.minSize,
        .defaultSize = contract.defaultSize,
        .draw =
            [&ui](editor_ui::PanelContext& context)
            {
                CloseLegacyBrowserSources(ui);

                static constexpr std::array<std::string_view, 2>
                    kBrowserModes{
                        "World",
                        "Assets"
                    };

                i32 browserIndex =
                    g_browserMode == BrowserMode::World
                        ? 0
                        : 1;

                if (context.SegmentedControl(
                        "world-assets-mode",
                        kBrowserModes,
                        browserIndex))
                {
                    g_browserMode =
                        browserIndex == 0
                            ? BrowserMode::World
                            : BrowserMode::Assets;
                }

                context.Separator();

                const std::string_view sourceTitle =
                    BrowserSourcePanelTitle(g_browserMode);

                if (!ui.DrawPanelContentsByTitle(
                        sourceTitle,
                        context))
                {
                    context.MutedText(
                        g_browserMode == BrowserMode::World
                            ? "World hierarchy is not registered in this Studio configuration."
                            : "Asset service is not registered in this Studio configuration.");
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

                if (g_workspaceMode == WorkspaceMode::Celestial)
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
        context.Button("v##bubble-open-" + suffix);
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

    context.Text("Celestial");
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
    context.SameLine();
    context.MutedText("|");
    context.SameLine();

    const auto toggle =
        [&](const schema::TypeId type,
            const std::string_view label)
        {
            const auto state =
                model.CapabilityEnabled(body->id, type);
            bool enabled = state.value_or(false);

            const std::string id =
                std::string(label) +
                "##celestial-tb-" +
                type.ToString();

            if (context.Checkbox(id, enabled))
            {
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

    toggle(world_model::kAtmosphereCapabilityType, "Atmosphere");
    toggle(world_model::kCloudLayerCapabilityType, "Clouds");
    toggle(world_model::kOceanCapabilityType, "Ocean");
    toggle(world_model::kRingSystemCapabilityType, "Rings");
    toggle(world_model::kMagnetosphereCapabilityType, "Aurora");
    toggle(world_model::kSurfaceCapabilityType, "Surface");

    if (context.Button("More...##celestial-tb-more"))
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

void StudioViewportPanels::DrawSceneToolbar(
    editor_ui::PanelContext& context)
{
    if (session_ == nullptr ||
        !session_->World().HasWorld())
    {
        context.MutedText(
            "Open a world to use the Scene tools.");
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
            context.SameLine();
            context.MutedText("|");
            context.SameLine();
        };

    context.Text("Scene");
    context.SameLine();

    static constexpr std::array<std::string_view, 4> kTools{
        "Select", "Move", "Rotate", "Scale"};
    i32 tool = static_cast<i32>(gizmo.tool);
    if (context.SegmentedControl(
            "scene-toolbar-gizmo-tool",
            kTools,
            tool))
    {
        gizmo.tool = static_cast<GizmoTool>(std::clamp(tool, 0, 3));
    }

    context.SameLine();
    static constexpr std::array<std::string_view, 2> kSpaces{
        "World", "Local"};
    i32 space = static_cast<i32>(gizmo.space);
    if (context.SegmentedControl(
            "scene-toolbar-space",
            kSpaces,
            space))
    {
        gizmo.space = static_cast<GizmoSpace>(std::clamp(space, 0, 1));
    }

    if (gizmo.tool != GizmoTool::Select)
    {
        context.SameLine();
        bool* snap = &gizmo.translationSnap;
        if (gizmo.tool == GizmoTool::Rotate)
        {
            snap = &gizmo.rotationSnap;
        }
        else if (gizmo.tool == GizmoTool::Scale)
        {
            snap = &gizmo.scaleSnap;
        }
        static_cast<void>(
            context.Checkbox("Snap##scene-toolbar-snap", *snap));
    }

    separator();
    context.MutedText("Create");
    context.SameLine();

    const bool canCreate = CanCreateAtViewport(kViewport);
    if (!canCreate)
    {
        context.MutedText("(focus a viewport)");
        context.SameLine();
    }
    else
    {
        if (context.Button("+ Point Light##scene-tb-point"))
        {
            run([&] { CreateLocalLightAtViewport(kViewport, false); });
        }
        context.SameLine();
        if (context.Button("+ Spot Light##scene-tb-spot"))
        {
            run([&] { CreateLocalLightAtViewport(kViewport, true); });
        }
        context.SameLine();
        if (context.Button("+ Box##scene-tb-box"))
        {
            run([&] { CreateVisibilityProxyAtViewport(kViewport, true); });
        }
        context.SameLine();
        if (context.Button("+ Sphere##scene-tb-sphere"))
        {
            run([&] { CreateVisibilityProxyAtViewport(kViewport, false); });
        }
        context.SameLine();
    }

    separator();
    context.MutedText("Edit");
    context.SameLine();

    const bool hasSelection = !world.Selection().Ordered().empty();
    if (context.Button("Frame Selected##scene-tb-frame") && hasSelection)
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
    if (context.Button("Duplicate##scene-tb-dup") && hasSelection)
    {
        run([&] { DuplicateSelection(); });
    }
    context.SameLine();
    if (context.Button("Delete##scene-tb-del") && hasSelection)
    {
        run([&] { DeleteSelection(); });
    }
    context.SameLine();
    if (context.Button("Undo##scene-tb-undo"))
    {
        run([&]
        {
            world.CommandRegistry().Invoke(
                editor_model::authoring_commands::kUndo);
        });
    }
    context.SameLine();
    if (context.Button("Redo##scene-tb-redo"))
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
        context.MutedText("Selected");
        context.SameLine();
        DrawElementBubble(
            context,
            world.Selection().Ordered().front());
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

    context.Text("Mode");
    context.SameLine();

    static constexpr std::array<std::string_view, 5> kWorkspaceModes{
        "Scene",
        "Planet",
        "Celestial",
        "Simulation",
        "Shading"
    };

    i32 workspace = static_cast<i32>(g_workspaceMode);
    if (context.Combo(
            "##workspace-mode-compact",
            kWorkspaceModes,
            workspace))
    {
        workspace = std::clamp(workspace, 0, 4);
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
    return g_workspaceMode != WorkspaceMode::Shading;
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
