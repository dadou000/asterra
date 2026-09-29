#include <orbit/studio_ui/StudioViewportPanels.hpp>

#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/studio_ui/StudioShellModel.hpp>
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <chrono>
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

#define DrawView DrawViewBase
#include "StudioViewportPanelsBase.cpp"
#undef DrawView

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

void CloseSpecialistPanels(editor_ui::EditorUi& ui)
{
    ClosePanels(
        ui,
        {
            "World / Assets",
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

void OpenCanonicalWorkspace(
    editor_ui::EditorUi& ui,
    const std::initializer_list<std::string_view> centerPanels)
{
    CloseSpecialistPanels(ui);
    ClosePanels(
        ui,
        {
            "Material Service",
            "Build",
            "Output"
        });

    OpenPanels(ui, {"Explorer"});
    OpenPanels(ui, centerPanels);
    OpenPanels(ui, {"Properties"});
}

void ActivateWorkspace(
    editor_ui::EditorUi& ui,
    const WorkspaceMode mode)
{
    g_workspaceMode = mode;
    g_browserMode = DefaultBrowserMode(mode);

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
                "Explorer",
                "Build",
                "Output"
            });
        OpenPanels(
            ui,
            {"Material Service", "Shading", "Properties"});
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
                static_cast<void>(
                    ui.ClosePanelByTitle("Explorer"));
                static_cast<void>(
                    ui.ClosePanelByTitle("Material Service"));

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
                "{} · {:.0f} lm",
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
        context.Text("2 Path Nodes · Connect");
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

    // Terrain context is owned exclusively by StudioExpansionShell. Keeping a
    // second terrain branch here used to duplicate the same tool choices and
    // made future toolbar changes easy to desynchronize.
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
        "Expert compatibility view · canonical editing lives in Properties.");

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
} // namespace orbit::studio_ui
