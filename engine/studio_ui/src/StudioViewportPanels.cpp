#include <orbit/studio_ui/StudioViewportPanels.hpp>

#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/studio_session/StudioTerrainAuthoringInvalidation.hpp>
#include <orbit/terrain_debug/TerrainDebugField.hpp>
#include <orbit/terrain_debug/TerrainDebugSeam.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] const char* ModeName(
    const studio_session::ViewportMode mode) noexcept
{
    switch (mode)
    {
    case studio_session::ViewportMode::Perspective:
        return "Perspective";
    case studio_session::ViewportMode::BodyMap:
        return "Body Map";
    case studio_session::ViewportMode::Debug:
        return "Debug";
    }

    return "Perspective";
}

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

[[nodiscard]] const char* EdgeName(
    const world::TileEdge edge) noexcept
{
    switch (edge)
    {
    case world::TileEdge::North:
        return "N";
    case world::TileEdge::East:
        return "E";
    case world::TileEdge::South:
        return "S";
    case world::TileEdge::West:
        return "W";
    }

    return "?";
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

[[nodiscard]] std::optional<paths::PathEdgeRecord>
SelectedBezierEdge(
    studio_session::StudioSession& session)
{
    if (!session.World().HasWorld())
    {
        return std::nullopt;
    }

    auto& paths = session.PathNetwork().Service();

    for (const auto object :
         session.World().Selection().Ordered())
    {
        const auto edge = paths.FindEdge(object);

        if (edge.has_value() &&
            edge->mode == paths::EdgeMode::Bezier)
        {
            return edge;
        }
    }

    return std::nullopt;
}
} // namespace

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

    context.Text(
        std::format(
            "Mode: {}",
            ModeName(target->mode)));

    const std::string perspectiveButton =
        "Perspective##" + std::string(id);
    const std::string mapButton =
        "Body Map##" + std::string(id);
    const std::string debugButton =
        "Debug##" + std::string(id);

    if (context.Button(perspectiveButton))
    {
        session_->Viewports().SetMode(
            id,
            studio_session::ViewportMode::Perspective);
    }
    context.SameLine();
    if (context.Button(mapButton))
    {
        session_->Viewports().SetMode(
            id,
            studio_session::ViewportMode::BodyMap);
    }
    context.SameLine();
    if (context.Button(debugButton))
    {
        session_->Viewports().SetMode(
            id,
            studio_session::ViewportMode::Debug);
    }

    if (target->mode !=
        studio_session::ViewportMode::Debug)
    {
        context.Separator();
        context.Text(
            std::format(
                "Terrain Tool: {}",
                TerrainToolName(
                    terrainTool_)));

        const auto setTool =
            [this, id](
                const StudioTerrainAuthoringTool tool)
            {
                if (terrainTool_ != tool &&
                    (IsSplineTool(terrainTool_) ||
                     IsSplineTool(tool)))
                {
                    terrainSplinePoints_.clear();
                    terrainSplineTerrain_.reset();
                }
                terrainTool_ = tool;
                views_->ClearTerrainAuthoringOverlay(id);
            };

        const std::string selectTool =
            "Select##terrain-tool-select:" +
            std::string(id);
        const std::string raiseTool =
            "Raise##terrain-tool-raise:" +
            std::string(id);
        const std::string lowerTool =
            "Lower##terrain-tool-lower:" +
            std::string(id);
        const std::string protectTool =
            "Protect##terrain-tool-protect:" +
            std::string(id);
        const std::string drainageTool =
            "Drainage##terrain-tool-drainage:" +
            std::string(id);
        const std::string canyonTool =
            "Canyon##terrain-tool-canyon:" +
            std::string(id);
        const std::string ridgeTool =
            "Ridge##terrain-tool-ridge:" +
            std::string(id);
        const std::string materialTool =
            "Geology##terrain-tool-material:" +
            std::string(id);
        const std::string biomePaintTool =
            "Biome Paint##terrain-tool-biome:" +
            std::string(id);

        if (context.Button(selectTool))
            setTool(StudioTerrainAuthoringTool::Select);
        context.SameLine();
        if (context.Button(raiseTool))
            setTool(StudioTerrainAuthoringTool::Raise);
        context.SameLine();
        if (context.Button(lowerTool))
            setTool(StudioTerrainAuthoringTool::Lower);
        context.SameLine();
        if (context.Button(protectTool))
            setTool(StudioTerrainAuthoringTool::Protection);

        if (context.Button(drainageTool))
            setTool(StudioTerrainAuthoringTool::Drainage);
        context.SameLine();
        if (context.Button(canyonTool))
            setTool(StudioTerrainAuthoringTool::Canyon);
        context.SameLine();
        if (context.Button(ridgeTool))
            setTool(StudioTerrainAuthoringTool::Ridge);
        context.SameLine();
        if (context.Button(materialTool))
            setTool(StudioTerrainAuthoringTool::Material);

        context.SameLine();
        if (context.Button(biomePaintTool))
            setTool(StudioTerrainAuthoringTool::BiomePaint);

        if (terrainTool_ !=
                StudioTerrainAuthoringTool::Select &&
            terrainTool_ !=
                StudioTerrainAuthoringTool::BiomePaint)
        {
            static_cast<void>(
                context.InputDouble(
                    "Inner Radius (m)##terrain-brush-inner",
                    terrainBrushInnerRadiusMeters_));
            static_cast<void>(
                context.InputDouble(
                    "Outer Radius (m)##terrain-brush-outer",
                    terrainBrushOuterRadiusMeters_));
        }

        if (terrainTool_ ==
                StudioTerrainAuthoringTool::Raise ||
            terrainTool_ ==
                StudioTerrainAuthoringTool::Lower)
        {
            static_cast<void>(
                context.InputDouble(
                    "Height Delta (m)##terrain-brush-height",
                    terrainBrushHeightMeters_));
        }
        else if (
            terrainTool_ ==
                StudioTerrainAuthoringTool::Protection)
        {
            static_cast<void>(
                context.InputDouble(
                    "Protection [0..1]##terrain-protection",
                    terrainProtection_));
        }
        else if (
            terrainTool_ ==
                StudioTerrainAuthoringTool::Drainage)
        {
            static_cast<void>(
                context.InputDouble(
                    "Drainage Guidance##terrain-drainage",
                    terrainDrainageGuidance_));
        }

        if (terrainTool_ ==
            StudioTerrainAuthoringTool::BiomePaint)
        {
            const auto selectedBiome =
                SelectedBiomeObject(*session_);

            if (selectedBiome.has_value())
            {
                const auto record =
                    session_->World().Objects().Find(
                        *selectedBiome);

                context.Text(
                    std::format(
                        "Biome: {}",
                        record.has_value()
                            ? record->name
                            : std::string("<missing>")));
            }
            else
            {
                context.Text(
                    "Biome: <select a Biome or one of its authored children>");
            }

            context.Text(
                std::format(
                    "Operation: {}",
                    BiomeOperationName(
                        biomePaintOperation_)));

            const std::string add =
                "Add##biome-paint-add:" +
                std::string(id);
            const std::string subtract =
                "Subtract##biome-paint-subtract:" +
                std::string(id);
            const std::string replace =
                "Replace##biome-paint-replace:" +
                std::string(id);

            if (context.Button(add))
            {
                biomePaintOperation_ =
                    terrain_biome::
                        BiomeAuthoredWeightOperation::Add;
            }
            context.SameLine();
            if (context.Button(subtract))
            {
                biomePaintOperation_ =
                    terrain_biome::
                        BiomeAuthoredWeightOperation::Subtract;
            }
            context.SameLine();
            if (context.Button(replace))
            {
                biomePaintOperation_ =
                    terrain_biome::
                        BiomeAuthoredWeightOperation::Replace;
            }

            static_cast<void>(
                context.InputDouble(
                    "Inner Radius (m)##biome-brush-inner",
                    biomeBrushInnerRadiusMeters_));
            static_cast<void>(
                context.InputDouble(
                    "Outer Radius (m)##biome-brush-outer",
                    biomeBrushOuterRadiusMeters_));
            static_cast<void>(
                context.InputDouble(
                    "Strength##biome-brush-value",
                    biomeBrushValue_));
            static_cast<void>(
                context.InputDouble(
                    "Opacity##biome-brush-opacity",
                    biomeBrushOpacity_));

            static_cast<void>(
                context.Checkbox(
                    "Automatic Placement Inspection##biome-auto-overlay",
                    biomeAutomaticOverlay_));

            if (hoveredBiomeAuthoredWeight_.has_value())
            {
                context.Text(
                    std::format(
                        "Authored weight under cursor: {:.4f}",
                        *hoveredBiomeAuthoredWeight_));
            }

            if (biomeAutomaticOverlay_)
            {
                if (hoveredBiomeAutomaticWeight_.has_value())
                {
                    context.Text(
                        std::format(
                            "Automatic weight under cursor: {:.4f}",
                            *hoveredBiomeAutomaticWeight_));
                }
                else
                {
                    context.Text(
                        "Automatic weight: unavailable when enabled selectors require unpublished physical fields.");
                }
            }
        }

        if (IsSplineTool(terrainTool_))
        {
            static_cast<void>(
                context.InputDouble(
                    "Spline Half Width (m)##terrain-spline-width",
                    terrainSplineHalfWidthMeters_));
            static_cast<void>(
                context.InputDouble(
                    "Spline Falloff (m)##terrain-spline-falloff",
                    terrainSplineFalloffMeters_));
            static_cast<void>(
                context.InputDouble(
                    terrainTool_ ==
                            StudioTerrainAuthoringTool::Canyon
                        ? "Canyon Depth (m)##terrain-spline-height"
                        : "Ridge Height (m)##terrain-spline-height",
                    terrainSplineHeightMeters_));

            context.Text(
                std::format(
                    "Control Points: {} (click terrain; double-click or Commit to finish)",
                    terrainSplinePoints_.size()));

            const std::string commitLabel =
                "Commit Spline##terrain-spline-commit:" +
                std::string(id);
            const std::string cancelLabel =
                "Cancel Spline##terrain-spline-cancel:" +
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

                        model.SelectObject(
                            constraint);

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
    }

    if (target->mode ==
        studio_session::ViewportMode::Debug)
    {
        context.Separator();
        context.Text("Terrain Debug Field");

        const auto selectedField =
            views_->DebugField(id);

        for (const auto& descriptor :
             terrain_debug::FieldCatalog())
        {
            const std::string label =
                std::string(descriptor.name) +
                "##terrain-debug:" +
                std::string(id) + ":" +
                std::to_string(
                    static_cast<u32>(
                        descriptor.field));

            if (context.Selectable(
                    label,
                    selectedField ==
                        descriptor.field))
            {
                views_->SetDebugField(
                    id,
                    descriptor.field);
                status_ =
                    "Debug field: " +
                    std::string(descriptor.name);
            }
        }

        const auto& descriptor =
            terrain_debug::Descriptor(
                views_->DebugField(id));

        i64 pageLevel =
            static_cast<i64>(
                views_->DebugPhysicalPageLevel(id));

        if (context.InputInteger(
                "Physical page tile level",
                pageLevel))
        {
            pageLevel =
                std::clamp<i64>(
                    pageLevel,
                    0,
                    30);
            views_->SetDebugPhysicalPageLevel(
                id,
                static_cast<u8>(pageLevel));
            status_ =
                "Physical page level changed; the center page will be reselected.";
        }

        const auto selectedPage =
            views_->DebugPhysicalPage(id);

        if (selectedPage.has_value())
        {
            const auto& tile =
                selectedPage->address.tile;
            context.Text(
                std::format(
                    "Physical page: {} L{} ({}, {})",
                    CubeFaceName(tile.face),
                    tile.level,
                    tile.x,
                    tile.y));
        }
        else
        {
            context.Text(
                "Physical page: <none> (click the planet in this viewport)");
        }

        const auto livePage =
            views_->LiveDebugPage(id);

        if (livePage != nullptr)
        {
            const auto& stamp =
                livePage->Stamp();
            const auto field =
                views_->DebugField(id);

            context.Text(
                std::format(
                    "Live products: ready (physical LOD {}, {}x{})",
                    stamp.physicalLod,
                    livePage->Width(),
                    livePage->Height()));
            context.Text(
                livePage->Has(field)
                    ? "Selected field: available"
                    : "Selected field: not published by the live page producer");

            if (livePage->Has(field))
            {
                const auto seams =
                    terrain_debug::
                        InspectTerrainDebugSeams(
                            *livePage,
                            field,
                            session_->
                                TerrainDebugPages());

                context.Text("Physical page seams");

                for (const auto& seam : seams)
                {
                    if (seam.state ==
                        terrain_debug::
                            TerrainDebugSeamState::
                                ValueMismatch)
                    {
                        context.Text(
                            std::format(
                                "{}: {} ({}/{} samples, max diff {:.6g})",
                                EdgeName(seam.edge),
                                terrain_debug::
                                    TerrainDebugSeamStateName(
                                        seam.state),
                                seam.comparison.
                                    mismatchedSamples,
                                seam.comparison.
                                    samplesCompared,
                                seam.comparison.
                                    maximumDifference));
                    }
                    else
                    {
                        context.Text(
                            std::format(
                                "{}: {}",
                                EdgeName(seam.edge),
                                terrain_debug::
                                    TerrainDebugSeamStateName(
                                        seam.state)));
                    }
                }

                context.Text(
                    "Seam border: green continuous | gray missing | blue LOD | yellow revision | magenta unavailable | red mismatch");
            }
        }
        else
        {
            context.Text(
                "Live products: no physical-page snapshot published yet");
        }

        context.Text(
            "Click the debug viewport image to select a physical terrain page.");

        context.Text("Upstream provenance");
        for (const auto stage :
             descriptor.upstream)
        {
            context.Text(
                std::format(
                    "- {}",
                    terrain_debug::StageName(stage)));
        }
    }

    const std::string followButton =
        "Follow Active Body##" + std::string(id);
    if (context.Button(followButton))
    {
        session_->Viewports().FollowActiveBody(id);
        status_ = "Viewport now follows the shared active body.";
    }

    if (target->target.has_value())
    {
        context.Text(
            std::format(
                "Target: {}",
                target->target->name));
        context.Text(
            std::format(
                "Body {}",
                target->target->body.ToString()));
        context.Text(
            std::format(
                "Frame {}",
                target->target->frame.ToString()));
    }
    else
    {
        context.Text("Target: <none>");
    }

    if (session_->World().HasWorld())
    {
        context.Separator();
        context.Text("Pin body");

        const auto& universe =
            session_->World().Universe();
        const auto& bodies = universe.Bodies();

        for (const auto systemId : bodies.Systems())
        {
            const auto* system =
                bodies.FindSystem(systemId);

            if (system != nullptr)
            {
                context.Text(system->name);
            }

            for (const auto bodyId :
                 bodies.Bodies(systemId))
            {
                const auto* body =
                    bodies.FindBody(bodyId);
                const auto semantic =
                    universe.ObjectForBody(bodyId);

                if (body == nullptr ||
                    !semantic.has_value())
                {
                    continue;
                }

                const bool selected =
                    target->target.has_value() &&
                    target->target->semanticObject ==
                        *semantic;
                const std::string label =
                    body->name +
                    "##viewport-body:" +
                    std::string(id) + ":" +
                    semantic->ToString();

                if (context.Selectable(label, selected))
                {
                    try
                    {
                        session_->Viewports().PinToObject(
                            id,
                            *semantic);
                        status_ =
                            "Viewport pinned to " +
                            body->name + ".";
                    }
                    catch (const std::exception& exception)
                    {
                        status_ = exception.what();
                    }
                }
            }
        }

        if (const auto edge = SelectedBezierEdge(*session_);
            edge.has_value())
        {
            context.Separator();
            context.Text("Bezier handles (metres, edge evaluation frame)");

            math::Double3 startHandle =
                edge->startHandleMeters;
            math::Double3 endHandle =
                edge->endHandleMeters;

            const std::string startLabel =
                "Start Handle##" + std::string(id);
            const std::string endLabel =
                "End Handle##" + std::string(id);

            bool changed =
                context.InputDouble3(
                    startLabel,
                    startHandle);
            changed =
                context.InputDouble3(
                    endLabel,
                    endHandle) ||
                changed;

            if (changed)
            {
                try
                {
                    session_->PathNetwork().
                        Service().SetBezierHandles(
                            edge->id,
                            startHandle,
                            endHandle);
                    status_ =
                        "Bezier handles updated. Undo/redo uses the shared command transaction stack.";
                }
                catch (const std::exception& exception)
                {
                    status_ = exception.what();
                }
            }
        }
    }

    if (target->mode ==
        studio_session::ViewportMode::Perspective)
    {
        context.Separator();
        context.Text("Terrain Diagnostics");

        auto diagnostics =
            views_->
                TerrainDiagnosticOverlays(
                    id);

        bool diagnosticsChanged =
            false;

        const auto toggle =
            [&](const char* name,
                bool& value)
            {
                const std::string label =
                    std::string(name) +
                    "##terrain-diagnostics:" +
                    std::string(id) + ":" +
                    name;

                diagnosticsChanged =
                    context.Checkbox(
                        label,
                        value) ||
                    diagnosticsChanged;
            };

        toggle(
            "Dirty page bounds",
            diagnostics.dirtyPageBounds);
        context.SameLine();
        toggle(
            "Build/upload states",
            diagnostics.buildStates);

        toggle(
            "Physical LOD",
            diagnostics.physicalLod);
        context.SameLine();
        toggle(
            "Clipmap rings",
            diagnostics.clipmapRings);

        toggle(
            "Cache status",
            diagnostics.cacheStatus);
        context.SameLine();
        toggle(
            "Authored constraints",
            diagnostics.authoredConstraints);

        toggle(
            "Biome weights",
            diagnostics.biomeWeights);
        context.SameLine();
        toggle(
            "Process masks",
            diagnostics.processMasks);

        toggle(
            "Drainage vectors",
            diagnostics.drainageVectors);

        if (diagnosticsChanged)
        {
            views_->
                SetTerrainDiagnosticOverlays(
                    id,
                    diagnostics);
        }

        if (diagnostics.cacheStatus)
        {
            const auto terrainRuntime =
                session_->
                    TerrainRuntime().
                    Capture(id);

            if (terrainRuntime.has_value())
            {
                const auto* services =
                    session_->
                        World().
                        Surfaces().
                        ServicesForBody(
                            terrainRuntime->body);

                if (services != nullptr)
                {
                    const auto stats =
                        services->
                            Cache().
                            Stats();

                    context.Text(
                        std::format(
                            "M26 cache: {} pages | {} bytes | hits {} | misses {} | evictions {}",
                            stats.residentPages,
                            stats.residentBytes,
                            stats.hits,
                            stats.misses,
                            stats.evictions));
                }

                const auto rebuild =
                    session_->
                        TerrainPhysicalPages().
                        BodyStatus(
                            terrainRuntime->
                                planet.id);

                if (rebuild.has_value())
                {
                    context.Text(
                        std::format(
                            "M06 pages: {} dirty | {} queued | {} building | {} uploading | {} ready",
                            rebuild->dirtyPages,
                            rebuild->queuedPages,
                            rebuild->buildingPages,
                            rebuild->uploadingPages,
                            rebuild->readyPages));
                }
            }
        }
    }

    if (!status_.empty())
    {
        context.Text(status_);
    }

    context.Separator();
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
            renderView->Color(),
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
