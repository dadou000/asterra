#include <orbit/studio_ui/StudioViewportPanels.hpp>

#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/CelestialAuthoringModel.hpp>
#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/studio_ui/CelestialAuthoringUi.hpp>
#include <orbit/studio_ui/SurfaceAuthoringUi.hpp>
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <exception>
#include <format>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#define Register RegisterBase
#define RegisterSecondary RegisterSecondaryBase
#define DrawView DrawViewBase
#include "StudioViewportPanelsBase.cpp"
#undef DrawView
#undef RegisterSecondary
#undef Register

namespace orbit::studio_ui
{
namespace
{
enum class WorkspaceMode : u8
{
    Scene,
    Planet,
    Celestial,
    Simulation,
    Shading
};

editor_ui::EditorUi* g_workspaceUi = nullptr;
StudioViewportPanels* g_shellPanels = nullptr;
WorkspaceMode g_workspaceMode = WorkspaceMode::Scene;

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

void OpenStandardInspectorWorkspace(
    editor_ui::EditorUi& ui,
    const std::initializer_list<std::string_view> centerPanels)
{
    CloseSpecialistPanels(ui);
    ClosePanels(
        ui,
        {
            "Properties",
            "Material Service",
            "Build",
            "Output"
        });

    OpenPanels(ui, {"Explorer"});
    OpenPanels(ui, centerPanels);
    OpenPanels(ui, {"Inspector"});
}

void ActivateWorkspace(
    editor_ui::EditorUi& ui,
    const WorkspaceMode mode)
{
    g_workspaceMode = mode;

    switch (mode)
    {
    case WorkspaceMode::Scene:
    case WorkspaceMode::Planet:
    case WorkspaceMode::Simulation:
        OpenStandardInspectorWorkspace(
            ui,
            {"Viewport"});
        break;

    case WorkspaceMode::Celestial:
        CloseSpecialistPanels(ui);
        ClosePanels(
            ui,
            {
                "Viewport",
                "Properties",
                "Material Service",
                "Build",
                "Output"
            });
        OpenPanels(
            ui,
            {"Explorer", "System View", "Inspector"});
        break;

    case WorkspaceMode::Shading:
        CloseSpecialistPanels(ui);
        ClosePanels(
            ui,
            {
                "Viewport",
                "Explorer",
                "Properties",
                "Material Service",
                "Build",
                "Output"
            });
        OpenPanels(
            ui,
            {"Shading", "Shading Materials", "Inspector"});
        break;
    }
}

void RegisterWorkspaceActions(editor_ui::EditorUi& ui)
{
    // Register() and RegisterSecondary() can both be used by the same Studio
    // shell. Keep one command surface instead of duplicating menu entries.
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
            ActivateWorkspace(
                ui,
                WorkspaceMode::Scene);
        }
    });

    ui.RegisterMenuAction({
        .menu = "Home",
        .label = "Workspace: Planet",
        .invoke = [&ui]
        {
            ActivateWorkspace(
                ui,
                WorkspaceMode::Planet);
        }
    });

    ui.RegisterMenuAction({
        .menu = "Home",
        .label = "Workspace: Celestial",
        .invoke = [&ui]
        {
            ActivateWorkspace(
                ui,
                WorkspaceMode::Celestial);
        }
    });

    ui.RegisterMenuAction({
        .menu = "Home",
        .label = "Workspace: Simulation",
        .invoke = [&ui]
        {
            ActivateWorkspace(
                ui,
                WorkspaceMode::Simulation);
        }
    });

    ui.RegisterMenuAction({
        .menu = "Home",
        .label = "Workspace: Shading",
        .invoke = [&ui]
        {
            ActivateWorkspace(
                ui,
                WorkspaceMode::Shading);
        }
    });
}

[[nodiscard]] const char* WorkspaceName(
    const WorkspaceMode mode) noexcept
{
    switch (mode)
    {
    case WorkspaceMode::Scene: return "Scene";
    case WorkspaceMode::Planet: return "Planet";
    case WorkspaceMode::Celestial: return "Celestial";
    case WorkspaceMode::Simulation: return "Simulation";
    case WorkspaceMode::Shading: return "Shading";
    }

    return "Scene";
}
} // namespace

StudioViewportPanels::StudioViewportPanels() = default;

StudioViewportPanels::~StudioViewportPanels()
{
    if (g_shellPanels == this)
    {
        static_cast<void>(
            editor_ui::RemoveShellBand("orbit.workspace"));
        static_cast<void>(
            editor_ui::RemoveShellBand("orbit.context"));
        static_cast<void>(
            editor_ui::RemoveShellBand("orbit.activity"));
        g_shellPanels = nullptr;
        g_workspaceUi = nullptr;
    }
}

void StudioViewportPanels::EnsureContextAuthoring()
{
    if (session_ == nullptr)
    {
        contextualSurface_.reset();
        contextualCelestial_.reset();
        contextualSession_ = nullptr;
        return;
    }

    if (contextualSession_ == session_ &&
        contextualSurface_ != nullptr &&
        contextualCelestial_ != nullptr)
    {
        return;
    }

    contextualSurface_ =
        std::make_unique<SurfaceAuthoringUi>(
            *session_);
    contextualCelestial_ =
        std::make_unique<CelestialAuthoringUi>(
            *session_);
    contextualSession_ =
        session_;
    contextualAdvancedProperties_ = false;
}

void StudioViewportPanels::RegisterShellBands(
    editor_ui::EditorUi& ui)
{
    g_shellPanels = this;
    g_workspaceUi = &ui;

    editor_ui::UpsertShellBand({
        .id = "orbit.workspace",
        .order = 0,
        .height = 40.0F,
        .draw =
            [](editor_ui::PanelContext& context)
            {
                if (g_shellPanels != nullptr)
                {
                    g_shellPanels->DrawWorkspaceBand(context);
                }
            }
    });

    editor_ui::UpsertShellBand({
        .id = "orbit.context",
        .order = 10,
        .height = 42.0F,
        .draw =
            [](editor_ui::PanelContext& context)
            {
                if (g_shellPanels != nullptr)
                {
                    g_shellPanels->DrawContextBand(context);
                }
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
}

void StudioViewportPanels::DrawWorkspaceBand(
    editor_ui::PanelContext& context)
{
    if (g_workspaceUi == nullptr)
    {
        return;
    }

    context.Text("Workspace");
    context.SameLine();

    const auto button =
        [&context](
            const char* name,
            const WorkspaceMode mode)
        {
            std::string label =
                g_workspaceMode == mode
                    ? std::string{"["} + name + "]"
                    : std::string{name};
            label += "##workspace-strip-";
            label += name;

            if (context.Button(label))
            {
                ActivateWorkspace(
                    *g_workspaceUi,
                    mode);
            }
        };

    button("Scene", WorkspaceMode::Scene);
    context.SameLine();
    button("Planet", WorkspaceMode::Planet);
    context.SameLine();
    button("Celestial", WorkspaceMode::Celestial);
    context.SameLine();
    button("Simulation", WorkspaceMode::Simulation);
    context.SameLine();
    button("Shading", WorkspaceMode::Shading);
}

void StudioViewportPanels::DrawContextBand(
    editor_ui::PanelContext& context)
{
    context.Text("Context");
    context.SameLine();

    if (session_ == nullptr ||
        !session_->World().HasWorld())
    {
        context.MutedText("Open a world to expose contextual tools.");
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
                session_->World().CommandRegistry().Invoke(
                    command);
                status_.clear();
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
            }
        };

    const auto setTerrainTool =
        [this](const StudioTerrainAuthoringTool tool)
        {
            if (terrainTool_ != tool &&
                (IsSplineTool(terrainTool_) ||
                 IsSplineTool(tool)))
            {
                terrainSplinePoints_.clear();
                terrainSplineTerrain_.reset();
            }

            terrainTool_ = tool;

            if (views_ != nullptr)
            {
                views_->ClearTerrainAuthoringOverlay(
                    "studio.primary");
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
        (selectedRecord->type ==
             world_model::kPointLightType ||
         selectedRecord->type ==
             world_model::kSpotLightType);

    const bool pathPairRelevant =
        world.CommandRegistry().Enablement(
            editor_model::authoring_commands::
                kConnectPathDirect).
            enabled;

    VolumeAuthoringUi* const volumeAuthoring =
        VolumeAuthoringUi::ContextInstance();
    const bool volumeRelevant =
        volumeAuthoring != nullptr &&
        volumeAuthoring->RelevantToSelection();

    bool surfaceRelevant = false;
    try
    {
        editor_model::SurfaceAuthoringModel surfaceModel(
            world.Objects(),
            world.Commands(),
            world.Selection());

        surfaceRelevant =
            surfaceModel.SelectedRockyBody().has_value();
    }
    catch (const std::exception&)
    {
        surfaceRelevant = false;
    }

    if (lightRelevant && renderView != nullptr)
    {
        const scene::ObjectId lightId =
            selectedRecord->id;
        const bool spot =
            selectedRecord->type ==
            world_model::kSpotLightType;

        f64 intensity = 1'000.0;
        if (const auto value =
                world.Objects().GetProperty(
                    lightId,
                    world_model::kLightIntensityLumens);
            value.has_value())
        {
            if (const auto* stored =
                    std::get_if<f64>(&*value);
                stored != nullptr)
            {
                intensity = *stored;
            }
        }

        bool enabled = true;
        if (const auto value =
                world.Objects().GetProperty(
                    lightId,
                    world_model::kLightEnabled);
            value.has_value())
        {
            if (const auto* stored =
                    std::get_if<bool>(&*value);
                stored != nullptr)
            {
                enabled = *stored;
            }
        }

        context.Text(
            std::format(
                "{} · {:.0f} lm",
                spot ? "Spot Light" : "Point Light",
                intensity));
        context.SameLine();

        const auto mutateLight =
            [this, lightId](
                const std::string_view name,
                const auto& mutation)
            {
                auto& commands =
                    session_->World().Commands();
                commands.BeginTransaction(
                    std::string(name));

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
                const auto& camera =
                    renderView->Camera();
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
                    const auto forward =
                        renderView->Camera().forward;
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
                const f64 next =
                    std::max(0.0, intensity / 1.25);
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
        context.Text("2 Path Nodes · Connect");
        context.SameLine();

        if (context.Button(
                "Direct##quick-path-direct"))
        {
            invokeAuthoringCommand(
                editor_model::authoring_commands::
                    kConnectPathDirect);
        }
        context.SameLine();
        if (context.Button(
                "Bezier##quick-path-bezier"))
        {
            invokeAuthoringCommand(
                editor_model::authoring_commands::
                    kConnectPathBezier);
        }
        context.SameLine();
        if (context.Button(
                "Routed##quick-path-routed"))
        {
            invokeAuthoringCommand(
                editor_model::authoring_commands::
                    kConnectPathRouted);
        }
        return;
    }

    if (volumeRelevant)
    {
        context.Text("Volume");
        context.SameLine();

        const auto sourceEnablement =
            world.CommandRegistry().Enablement(
                editor_model::authoring_commands::
                    kAddVolumeSource);
        const auto effectorEnablement =
            world.CommandRegistry().Enablement(
                editor_model::authoring_commands::
                    kAddVolumeEffector);

        bool drewButton = false;
        if (sourceEnablement.enabled)
        {
            if (context.Button(
                    "Add Source##quick-volume-source"))
            {
                invokeAuthoringCommand(
                    editor_model::authoring_commands::
                        kAddVolumeSource);
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
                    editor_model::authoring_commands::
                        kAddVolumeEffector);
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
                g_workspaceUi->FocusPanelByTitle(
                    "Volumes"));
        }
        return;
    }

    if (surfaceRelevant)
    {
        context.Text("Terrain");
        context.SameLine();

        if (context.Button(
                "Select##quick-terrain-select"))
            setTerrainTool(StudioTerrainAuthoringTool::Select);
        context.SameLine();
        if (context.Button(
                "Raise##quick-terrain-raise"))
            setTerrainTool(StudioTerrainAuthoringTool::Raise);
        context.SameLine();
        if (context.Button(
                "Lower##quick-terrain-lower"))
            setTerrainTool(StudioTerrainAuthoringTool::Lower);
        context.SameLine();
        if (context.Button(
                "Protect##quick-terrain-protect"))
            setTerrainTool(StudioTerrainAuthoringTool::Protection);
        context.SameLine();
        if (context.Button(
                "Drainage##quick-terrain-drainage"))
            setTerrainTool(StudioTerrainAuthoringTool::Drainage);
        context.SameLine();
        if (context.Button(
                "Canyon##quick-terrain-canyon"))
            setTerrainTool(StudioTerrainAuthoringTool::Canyon);
        context.SameLine();
        if (context.Button(
                "Ridge##quick-terrain-ridge"))
            setTerrainTool(StudioTerrainAuthoringTool::Ridge);
        context.SameLine();
        if (context.Button(
                "Geology##quick-terrain-material"))
            setTerrainTool(StudioTerrainAuthoringTool::Material);
        context.SameLine();
        if (context.Button(
                "Biome Paint##quick-terrain-biome"))
            setTerrainTool(StudioTerrainAuthoringTool::BiomePaint);
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
                g_workspaceUi->FocusPanelByTitle(
                    "Volumes"));
        }
        break;
    case WorkspaceMode::Celestial:
    case WorkspaceMode::Shading:
        context.MutedText(
            std::format(
                "{} tools are available in the active workspace and Inspector.",
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

    const auto panels =
        g_workspaceUi->Panels();

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
                        return candidate.title == title;
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
                                g_workspaceUi->ClosePanelByTitle(
                                    title));
                        }
                        else
                        {
                            static_cast<void>(
                                g_workspaceUi->FocusPanelByTitle(
                                    title));
                        }
                    }
            });
        };

    // These labels are intentionally presentation aliases. Output remains the
    // authoritative log panel and Build remains the authoritative build view;
    // the activity strip only opens/focuses/collapses them.
    appendPanel("Output", "Console");
    appendPanel("Build", "Build");
    appendPanel("Tasks", "Tasks");
    appendPanel("Display Diagnostics", "Diagnostics");

    context.Text("Activity");

    if (actions.empty())
    {
        context.SameLine();
        context.MutedText("No activity views registered.");
        return;
    }

    context.SameLine();
    context.Toolbar(actions);
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
        .defaultOpen = true,
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
    EnsureContextAuthoring();

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

    if (selected.empty())
    {
        context.Text("No selection");
        context.MutedText(
            "Select an object in Explorer or the active workspace. Relevant authoring controls appear here automatically.");
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

                    if constexpr (
                        std::is_same_v<Value, bool>)
                    {
                        changed =
                            context.Checkbox(label, value);
                    }
                    else if constexpr (
                        std::is_same_v<Value, i64>)
                    {
                        changed =
                            context.InputInteger(label, value);
                    }
                    else if constexpr (
                        std::is_same_v<Value, f64>)
                    {
                        changed =
                            context.InputDouble(label, value);
                    }
                    else if constexpr (
                        std::is_same_v<Value, std::string>)
                    {
                        changed =
                            context.InputText(label, value);
                    }
                    else if constexpr (
                        std::is_same_v<Value, math::Double3>)
                    {
                        changed =
                            context.InputDouble3(label, value);
                    }
                    else if constexpr (
                        requires { value.high; value.low; })
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

    bool surfaceRelevant = false;
    bool celestialRelevant = false;

    if (selected.size() == 1U)
    {
        try
        {
            editor_model::SurfaceAuthoringModel surfaceModel(
                world.Objects(),
                world.Commands(),
                world.Selection());

            surfaceRelevant =
                surfaceModel.SelectedRockyBody().has_value();
        }
        catch (const std::exception&)
        {
            surfaceRelevant = false;
        }

        try
        {
            editor_model::CelestialAuthoringModel celestialModel(
                world.Objects(),
                world.Schemas(),
                world.Commands(),
                world.Selection());

            const auto primary =
                celestialModel.PrimarySelection();

            celestialRelevant =
                celestialModel.SelectedBody().has_value() ||
                (primary.has_value() &&
                 (primary->type ==
                      world_model::kCelestialSystemType ||
                  primary->type ==
                      world_model::kCelestialReferenceNodeType ||
                  primary->type ==
                      world_model::kRingBandType ||
                  celestialModel.IsCapabilityType(
                      primary->type)));
        }
        catch (const std::exception&)
        {
            celestialRelevant = false;
        }
    }

    VolumeAuthoringUi* const volumeAuthoring =
        VolumeAuthoringUi::ContextInstance();
    const bool volumeRelevant =
        volumeAuthoring != nullptr &&
        volumeAuthoring->RelevantToSelection();

    if (surfaceRelevant &&
        contextualSurface_ != nullptr &&
        context.Section(
            "Surface##context-inspector-surface",
            true))
    {
        contextualSurface_->Draw(context);
    }

    if (celestialRelevant &&
        contextualCelestial_ != nullptr &&
        context.Section(
            "Celestial##context-inspector-celestial",
            !surfaceRelevant))
    {
        contextualCelestial_->Draw(context);
    }

    if (volumeRelevant &&
        context.Section(
            "Volume##context-inspector-volume",
            !surfaceRelevant &&
                !celestialRelevant))
    {
        volumeAuthoring->Draw(context);
    }

    if (!surfaceRelevant &&
        !celestialRelevant &&
        !volumeRelevant)
    {
        context.MutedText(
            "No specialized authoring section is needed for this selection. Common schema properties remain fully editable above.");
    }

    if (!status_.empty())
    {
        context.Separator();
        context.Text(status_);
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

    RegisterContextInspector(ui);
    RegisterWorkspaceActions(ui);
    RegisterShellBands(ui);
}

void StudioViewportPanels::DrawView(
    editor_ui::PanelContext& context,
    const std::string_view id)
{
    // Camera, picking, terrain painting, overlays and detailed viewport state
    // stay in the production viewport implementation. Workspace navigation and
    // high-frequency selection actions now live permanently in shell bands.
    DrawViewBase(context, id);
}
} // namespace orbit::studio_ui
