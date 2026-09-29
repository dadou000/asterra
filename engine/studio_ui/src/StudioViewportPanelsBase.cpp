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

#pragma push_macro("Register")
#undef Register
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
#pragma pop_macro("Register")
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
    {
        const std::string_view id =
            expansion_.ControlledViewportId();
        if (session_->Viewports().Find(id) == nullptr)
        {
            throw std::logic_error(
                "Controlled viewport is unavailable.");
        }
        session_->Viewports().SetMode(
            id,
            studio_session::ViewportMode::System);
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
        .defaultDock = orbit::editor_ui::DockRegion::Center,
        .dockOrder = 0,
        .minSize = {.width = 320.0F, .height = 200.0F},
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawView(context, "studio.primary");
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
                DrawView(context, "studio.map");
            }
    });
}

void StudioViewportPanels::RegisterSecondary(
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
                DrawView(context, "studio.map");
            }
    });
}

void StudioViewportPanels::DrawView(
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

    if (imageInteraction.clicked)
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
