#include <orbit/studio_ui/StudioExpansionShell.hpp>

#include <orbit/content/ContentService.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/editor_ui/FocusState.hpp>
#include <orbit/editor_ui/PanelExtensions.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/studio_ui/CommandPaletteModel.hpp>
#include <orbit/studio_ui/SelectionBreadcrumbs.hpp>
#include <orbit/studio_ui/StudioViewportPanels.hpp>
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>
#include <orbit/terrain_debug/TerrainDebugField.hpp>
#include <orbit/terrain_debug/TerrainDebugSeam.hpp>
#include <orbit/world_model/VisibilityProxyBinding.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "StudioShellInternals.hpp"

namespace orbit::studio_ui
{
using namespace shell_detail;

void StudioExpansionShell::DrawViewportBand(
    editor_ui::PanelContext& context)
{
    SyncPersistentState();

    // Selection-driven authoring is the primary content of row two. Generic
    // viewport/gizmo state follows it compactly rather than living in a fourth
    // permanent toolbar. Terrain gets one selector here; only its active
    // parameters are disclosed in Properties.
    if (owner_ != nullptr)
    {
        if (TerrainContextRelevant())
        {
            DrawTerrainContext(context);
        }
        else
        {
            owner_->DrawContextBand(context);
        }
    }
    else
    {
        context.Text("Context");
    }

    context.SameLine();
    context.MutedText("|");
    context.SameLine();
    context.Text("View");

    // Auto mode follows focus, so the target does not need permanent toolbar
    // chrome. Only surface a compact cue when the user explicitly pins the
    // control target in Properties.
    if (viewportControlMode_ != 0)
    {
        context.SameLine();
        context.MutedText(
            viewportControlMode_ == 2
                ? "Pinned Map"
                : "Pinned P");
    }

    if (ViewportControlsRelevant())
    {
        const std::string_view id = SelectedViewportId();
        const auto* target = owner_->session_->Viewports().Find(id);

        if (target != nullptr)
        {
            static constexpr std::array<std::string_view, 5>
                kViewportModes{
                    "Perspective",
                    "Body Map",
                    "Debug",
                    "System",
                    "Flat Map"
                };
            static constexpr std::array<studio_session::ViewportMode, 5>
                kViewportModeValues{
                    studio_session::ViewportMode::Perspective,
                    studio_session::ViewportMode::BodyMap,
                    studio_session::ViewportMode::Debug,
                    studio_session::ViewportMode::System,
                    studio_session::ViewportMode::FlatMap
                };

            i32 viewportMode = 0;
            switch (target->mode)
            {
            case studio_session::ViewportMode::Perspective:
                viewportMode = 0;
                break;
            case studio_session::ViewportMode::BodyMap:
                viewportMode = 1;
                break;
            case studio_session::ViewportMode::Debug:
                viewportMode = 2;
                break;
            case studio_session::ViewportMode::System:
                viewportMode = 3;
                break;
            case studio_session::ViewportMode::FlatMap:
                viewportMode = 4;
                break;
            }

            context.SameLine();
            if (context.Combo(
                    "##viewport-mode-compact",
                    kViewportModes,
                    viewportMode))
            {
                viewportMode = std::clamp(viewportMode, 0, 4);
                try
                {
                    owner_->InvokeViewportMode(
                        kViewportModeValues[
                            static_cast<std::size_t>(viewportMode)]);
                    owner_->status_.clear();
                    target = owner_->session_->Viewports().Find(id);
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }

            switch (target->mode)
            {
            case studio_session::ViewportMode::Perspective:
            {
                context.SameLine();
                i32 surface = static_cast<i32>(
                    owner_->views_->SurfaceDebugMode(id));
                if (context.Combo(
                        "##surface-debug-mode",
                        kSurfaceViews,
                        surface))
                {
                    surface = std::clamp(surface, 0, 3);
                    owner_->views_->SetSurfaceDebugMode(
                        id,
                        static_cast<lighting::SurfaceDebugMode>(surface));
                }
                break;
            }

            case studio_session::ViewportMode::Debug:
            {
                context.SameLine();
                const auto catalog = terrain_debug::FieldCatalog();
                std::vector<std::string_view> names;
                names.reserve(catalog.size());

                i32 selected = 0;
                const auto field = owner_->views_->DebugField(id);
                for (std::size_t index = 0U; index < catalog.size(); ++index)
                {
                    names.push_back(catalog[index].name);
                    if (catalog[index].field == field)
                    {
                        selected = static_cast<i32>(index);
                    }
                }

                if (!names.empty() &&
                    context.Combo(
                        "##viewport-debug-field-compact",
                        names,
                        selected))
                {
                    selected = std::clamp<i32>(
                        selected,
                        0,
                        static_cast<i32>(catalog.size()) - 1);
                    owner_->views_->SetDebugField(
                        id,
                        catalog[static_cast<std::size_t>(selected)].field);
                }
                break;
            }

            case studio_session::ViewportMode::FlatMap:
            {
                context.SameLine();
                static constexpr std::array<std::string_view, 5>
                    kMapLayers{
                        "Elevation",
                        "Biomes",
                        "Temperature",
                        "Precipitation",
                        "Water depth"
                    };
                i32 layer = static_cast<i32>(
                    owner_->views_->FlatMapLayerOf(id));
                if (context.Combo(
                        "##flat-map-layer",
                        kMapLayers,
                        layer))
                {
                    layer = std::clamp(layer, 0, 4);
                    owner_->views_->SetFlatMapLayer(
                        id,
                        static_cast<FlatMapLayer>(layer));
                }
                break;
            }

            case studio_session::ViewportMode::BodyMap:
            case studio_session::ViewportMode::System:
                break;
            }
        }
    }

    context.SameLine();

    static constexpr std::array<std::string_view, 4>
        kTools{"Select", "Move", "Rotate", "Scale"};
    i32 tool = static_cast<i32>(viewportState_.gizmo.tool);
    if (context.SegmentedControl(
            "viewport-gizmo-tool",
            kTools,
            tool))
    {
        viewportState_.gizmo.tool =
            static_cast<GizmoTool>(tool);
    }

    switch (viewportState_.gizmo.tool)
    {
    case GizmoTool::Select:
        break;

    case GizmoTool::Translate:
    {
        context.SameLine();
        static constexpr std::array<std::string_view, 4>
            kTranslationSnapModes{"Off", "Grid", "Surface", "Both"};
        i32 snapMode =
            (viewportState_.gizmo.translationSnap ? 1 : 0) |
            (viewportState_.gizmo.surfaceSnap ? 2 : 0);
        if (context.Combo(
                "##gizmo-translation-snap-mode",
                kTranslationSnapModes,
                snapMode))
        {
            viewportState_.gizmo.translationSnap =
                (snapMode & 1) != 0;
            viewportState_.gizmo.surfaceSnap =
                (snapMode & 2) != 0;
        }
        break;
    }

    case GizmoTool::Rotate:
    {
        context.SameLine();
        static constexpr std::array<std::string_view, 2>
            kRotationSnapModes{"Off", "Angle"};
        i32 snapMode = viewportState_.gizmo.rotationSnap ? 1 : 0;
        if (context.Combo(
                "##gizmo-rotation-snap-mode",
                kRotationSnapModes,
                snapMode))
        {
            viewportState_.gizmo.rotationSnap = snapMode != 0;
        }
        break;
    }

    case GizmoTool::Scale:
    {
        context.SameLine();
        static constexpr std::array<std::string_view, 2>
            kScaleSnapModes{"Off", "Step"};
        i32 snapMode = viewportState_.gizmo.scaleSnap ? 1 : 0;
        if (context.Combo(
                "##gizmo-scale-snap-mode",
                kScaleSnapModes,
                snapMode))
        {
            viewportState_.gizmo.scaleSnap = snapMode != 0;
        }
        break;
    }
    }

    static_cast<void>(DrawContributions(
        context,
        StudioContributionSurface::ContextToolbar,
        true));
}
} // namespace orbit::studio_ui
