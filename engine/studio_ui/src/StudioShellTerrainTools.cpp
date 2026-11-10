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

bool StudioExpansionShell::TerrainContextRelevant() const noexcept
{
    if (owner_ == nullptr ||
        owner_->session_ == nullptr ||
        !owner_->session_->World().HasWorld())
    {
        return false;
    }

    auto& world = owner_->session_->World();
    const auto& selection = world.Selection().Ordered();

    // Preserve the existing context-row precedence. Lights, path-pair actions
    // and volume authoring remain more specific than terrain ancestry.
    if (selection.size() == 1U)
    {
        const auto record =
            world.Objects().Find(selection.front());
        if (record.has_value() &&
            (record->type == world_model::kPointLightType ||
             record->type == world_model::kSpotLightType))
        {
            return false;
        }
    }

    if (world.CommandRegistry().Enablement(
            editor_model::authoring_commands::
                kConnectPathDirect).enabled)
    {
        return false;
    }

    if (auto* volume = VolumeAuthoringUi::ContextInstance();
        volume != nullptr &&
        volume->RelevantToSelection())
    {
        return false;
    }

    try
    {
        editor_model::SurfaceAuthoringModel model(
            world.Objects(),
            world.Commands(),
            world.Selection());
        return model.SelectedRockyBody().has_value();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

void StudioExpansionShell::DrawTerrainContext(
    editor_ui::PanelContext& context)
{
    if (owner_ == nullptr)
    {
        return;
    }

    context.Text("Terrain");
    context.SameLine();

    i32 tool =
        static_cast<i32>(owner_->terrainTool_);

    if (context.Combo(
            "##quick-terrain-tool",
            kTerrainTools,
            tool))
    {
        const auto next =
            static_cast<StudioTerrainAuthoringTool>(tool);

        if (owner_->terrainTool_ != next &&
            (IsTerrainSplineTool(owner_->terrainTool_) ||
             IsTerrainSplineTool(next)))
        {
            owner_->terrainSplinePoints_.clear();
            owner_->terrainSplineTerrain_.reset();
        }

        owner_->terrainTool_ = next;

        if (owner_->views_ != nullptr)
        {
            owner_->views_->ClearTerrainAuthoringOverlay(
                "studio.primary");
        }
    }

    if (IsTerrainSplineTool(owner_->terrainTool_))
    {
        context.SameLine();
        context.MutedText(
            std::format(
                "{} pts",
                owner_->terrainSplinePoints_.size()));
    }
}

void StudioExpansionShell::DrawTerrainToolProperties(
    editor_ui::PanelContext& context)
{
    if (owner_ == nullptr ||
        owner_->session_ == nullptr)
    {
        return;
    }

    i32 tool =
        static_cast<i32>(owner_->terrainTool_);

    if (context.Combo(
            "Tool##active-terrain-tool",
            kTerrainTools,
            tool))
    {
        const auto next =
            static_cast<StudioTerrainAuthoringTool>(tool);

        if (owner_->terrainTool_ != next &&
            (IsTerrainSplineTool(owner_->terrainTool_) ||
             IsTerrainSplineTool(next)))
        {
            owner_->terrainSplinePoints_.clear();
            owner_->terrainSplineTerrain_.reset();
        }

        owner_->terrainTool_ = next;

        if (owner_->views_ != nullptr)
        {
            owner_->views_->ClearTerrainAuthoringOverlay(
                "studio.primary");
        }
    }

    switch (owner_->terrainTool_)
    {
    case StudioTerrainAuthoringTool::Select:
        context.MutedText(
            "Selection mode has no brush parameters. Click terrain to select and inspect it.");
        return;

    case StudioTerrainAuthoringTool::Raise:
    case StudioTerrainAuthoringTool::Lower:
    case StudioTerrainAuthoringTool::Protection:
    case StudioTerrainAuthoringTool::Drainage:
    case StudioTerrainAuthoringTool::Material:
        static_cast<void>(context.InputDouble(
            "Inner Radius (m)##active-terrain-inner",
            owner_->terrainBrushInnerRadiusMeters_));
        static_cast<void>(context.InputDouble(
            "Outer Radius (m)##active-terrain-outer",
            owner_->terrainBrushOuterRadiusMeters_));

        owner_->terrainBrushInnerRadiusMeters_ =
            std::max(0.0, owner_->terrainBrushInnerRadiusMeters_);
        owner_->terrainBrushOuterRadiusMeters_ =
            std::max(
                owner_->terrainBrushInnerRadiusMeters_,
                owner_->terrainBrushOuterRadiusMeters_);

        if (owner_->terrainTool_ ==
                StudioTerrainAuthoringTool::Raise ||
            owner_->terrainTool_ ==
                StudioTerrainAuthoringTool::Lower)
        {
            static_cast<void>(context.InputDouble(
                "Height Delta (m)##active-terrain-height",
                owner_->terrainBrushHeightMeters_));
        }
        else if (owner_->terrainTool_ ==
                 StudioTerrainAuthoringTool::Protection)
        {
            static_cast<void>(context.SliderDouble(
                "Protection##active-terrain-protection",
                owner_->terrainProtection_,
                0.0,
                1.0));
        }
        else if (owner_->terrainTool_ ==
                 StudioTerrainAuthoringTool::Drainage)
        {
            static_cast<void>(context.InputDouble(
                "Drainage Guidance##active-terrain-drainage",
                owner_->terrainDrainageGuidance_));
        }
        else
        {
            context.MutedText(
                "Geology painting currently writes the body's default bedrock material.");
        }
        return;

    case StudioTerrainAuthoringTool::DrainagePath:
        static_cast<void>(context.InputDouble(
            "Half Width (m)##active-drainage-spline-width",
            owner_->terrainSplineHalfWidthMeters_));
        static_cast<void>(context.InputDouble(
            "Falloff (m)##active-drainage-spline-falloff",
            owner_->terrainSplineFalloffMeters_));
        static_cast<void>(context.SliderDouble(
            "Drainage Guidance##active-drainage-spline-guidance",
            owner_->terrainDrainageGuidance_,
            0.0,
            1.0));
        owner_->terrainSplineHalfWidthMeters_ =
            std::max(0.0, owner_->terrainSplineHalfWidthMeters_);
        owner_->terrainSplineFalloffMeters_ =
            std::max(0.0, owner_->terrainSplineFalloffMeters_);
        context.MutedText(
            "Guides downhill routing within this corridor; it does not force water uphill.");
        context.MutedText(
            "River channels remain generated from the drainage field; direct node editing is not available yet.");
        context.Text(
            std::format(
                "Control points: {} · click terrain to add; double-click to commit.",
                owner_->terrainSplinePoints_.size()));
        if (!owner_->terrainSplinePoints_.empty() &&
            context.Button("Cancel Drainage Path##active-drainage-spline-cancel"))
        {
            owner_->terrainSplinePoints_.clear();
            owner_->terrainSplineTerrain_.reset();
            if (owner_->views_ != nullptr)
                owner_->views_->ClearTerrainAuthoringOverlay("studio.primary");
            owner_->status_ = "Transient drainage path cancelled.";
        }
        return;

    case StudioTerrainAuthoringTool::Canyon:
    case StudioTerrainAuthoringTool::Ridge:
        static_cast<void>(context.InputDouble(
            "Half Width (m)##active-terrain-spline-width",
            owner_->terrainSplineHalfWidthMeters_));
        static_cast<void>(context.InputDouble(
            "Falloff (m)##active-terrain-spline-falloff",
            owner_->terrainSplineFalloffMeters_));
        static_cast<void>(context.InputDouble(
            owner_->terrainTool_ == StudioTerrainAuthoringTool::Canyon
                ? "Depth (m)##active-terrain-spline-height"
                : "Height (m)##active-terrain-spline-height",
            owner_->terrainSplineHeightMeters_));

        owner_->terrainSplineHalfWidthMeters_ =
            std::max(0.0, owner_->terrainSplineHalfWidthMeters_);
        owner_->terrainSplineFalloffMeters_ =
            std::max(0.0, owner_->terrainSplineFalloffMeters_);

        context.Text(
            std::format(
                "Control points: {} · click terrain to add; double-click to commit.",
                owner_->terrainSplinePoints_.size()));

        if (!owner_->terrainSplinePoints_.empty() &&
            context.Button(
                "Cancel Spline##active-terrain-spline-cancel"))
        {
            owner_->terrainSplinePoints_.clear();
            owner_->terrainSplineTerrain_.reset();
            if (owner_->views_ != nullptr)
            {
                owner_->views_->ClearTerrainAuthoringOverlay(
                    "studio.primary");
            }
            owner_->status_ =
                "Transient terrain spline cancelled.";
        }
        return;

    case StudioTerrainAuthoringTool::BiomePaint:
    {
        i32 operation =
            static_cast<i32>(owner_->biomePaintOperation_);
        operation = std::clamp(
            operation,
            0,
            static_cast<i32>(kBiomeOperations.size()) - 1);

        if (context.Combo(
                "Operation##active-biome-operation",
                kBiomeOperations,
                operation))
        {
            owner_->biomePaintOperation_ =
                static_cast<terrain_biome::
                    BiomeAuthoredWeightOperation>(operation);
        }

        static_cast<void>(context.InputDouble(
            "Inner Radius (m)##active-biome-inner",
            owner_->biomeBrushInnerRadiusMeters_));
        static_cast<void>(context.InputDouble(
            "Outer Radius (m)##active-biome-outer",
            owner_->biomeBrushOuterRadiusMeters_));
        static_cast<void>(context.SliderDouble(
            "Strength##active-biome-strength",
            owner_->biomeBrushValue_,
            0.0,
            1.0));
        static_cast<void>(context.SliderDouble(
            "Opacity##active-biome-opacity",
            owner_->biomeBrushOpacity_,
            0.0,
            1.0));
        static_cast<void>(context.Checkbox(
            "Automatic Placement Inspection##active-biome-auto",
            owner_->biomeAutomaticOverlay_));

        owner_->biomeBrushInnerRadiusMeters_ =
            std::max(0.0, owner_->biomeBrushInnerRadiusMeters_);
        owner_->biomeBrushOuterRadiusMeters_ =
            std::max(
                owner_->biomeBrushInnerRadiusMeters_,
                owner_->biomeBrushOuterRadiusMeters_);

        if (owner_->hoveredBiomeAuthoredWeight_.has_value())
        {
            context.Text(
                std::format(
                    "Authored weight under cursor: {:.4f}",
                    *owner_->hoveredBiomeAuthoredWeight_));
        }

        if (owner_->biomeAutomaticOverlay_)
        {
            if (owner_->hoveredBiomeAutomaticWeight_.has_value())
            {
                context.Text(
                    std::format(
                        "Automatic weight under cursor: {:.4f}",
                        *owner_->hoveredBiomeAutomaticWeight_));
            }
            else
            {
                context.MutedText(
                    "Automatic placement weight is unavailable for the current selectors/cursor.");
            }
        }
        return;
    }
    }
}
} // namespace orbit::studio_ui
