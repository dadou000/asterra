#include "StudioShellInternals.hpp"

namespace orbit::studio_ui::shell_detail
{
InspectorProviderRegistry g_inspectorProviders;
StudioUiContributionRegistry g_uiContributions;
}

namespace orbit::studio_ui
{
using namespace shell_detail;

InspectorProviderRegistry& GlobalInspectorProviders() noexcept
{
    return g_inspectorProviders;
}

StudioUiContributionRegistry& GlobalStudioUiContributions() noexcept
{
    return g_uiContributions;
}

StudioExpansionShell::StudioExpansionShell(
    StudioViewportPanels& owner) noexcept
    : owner_(&owner)
{
    try
    {
        // Two top rows only: workspace/navigation and contextual authoring.
        // The bottom activity strip is owned by StudioViewportPanels because
        // it directly controls that object's build/log/diagnostic panels.
        editor_ui::UpsertShellBand({
            .id = "orbit.navigation",
            .order = 0,
            .height = 40.0F,
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawNavigationBand(context);
                }
        });

        editor_ui::UpsertShellBand({
            .id = "orbit.viewport-authoring",
            .order = 10,
            .height = 42.0F,
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawViewportBand(context);
                }
        });

        // Plugin/context providers extend the same canonical Properties panel
        // as built-in authoring tools. The legacy Inspector remains available
        // as an expert compatibility surface, but it is no longer a second
        // default property-editing destination.
        editor_ui::UpsertPanelExtension({
            .id = "orbit.inspector.providers",
            .targetTitle = "Properties",
            .order = 1'100,
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawInspectorExtension(context);
                }
        });

        GlobalInspectorProviders().Upsert({
            .id = "orbit.viewport.target",
            .owner = "orbit",
            .title = "Viewport Target",
            .order = 20,
            .defaultOpen = false,
            .relevant =
                [this]
                {
                    return ViewportControlsRelevant();
                },
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawViewportTargetProperties(context);
                }
        });

        GlobalInspectorProviders().Upsert({
            .id = "orbit.path.bezier",
            .owner = "orbit",
            .title = "Bezier Path",
            .order = 60,
            .defaultOpen = true,
            .relevant =
                [this]
                {
                    return BezierContextRelevant();
                },
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawBezierProperties(context);
                }
        });

        // Terrain used to occupy most of the permanent context row with nine
        // buttons. Keep one selector in the shell and move only the active
        // tool's parameters into canonical Properties.
        GlobalInspectorProviders().Upsert({
            .id = "orbit.terrain.active-tool",
            .owner = "orbit",
            .title = "Terrain Tool",
            .order = 80,
            .defaultOpen = true,
            .relevant =
                [this]
                {
                    return TerrainContextRelevant();
                },
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawTerrainToolProperties(context);
                }
        });

        GlobalInspectorProviders().Upsert({
            .id = "orbit.viewport.diagnostics",
            .owner = "orbit",
            .title = "Viewport Diagnostics",
            .order = 120,
            .defaultOpen = false,
            .relevant =
                [this]
                {
                    return ViewportControlsRelevant();
                },
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawViewportDiagnosticsProperties(context);
                }
        });

        attached_ = true;
    }
    catch (...)
    {
        // Registration is transactional from the shell owner's point of view:
        // never leave callbacks retaining this object after a partial setup.
        static_cast<void>(
            GlobalInspectorProviders().Remove(
                "orbit.viewport.diagnostics"));
        static_cast<void>(
            GlobalInspectorProviders().Remove(
                "orbit.terrain.active-tool"));
        static_cast<void>(
            GlobalInspectorProviders().Remove(
                "orbit.path.bezier"));
        static_cast<void>(
            GlobalInspectorProviders().Remove(
                "orbit.viewport.target"));
        static_cast<void>(
            editor_ui::RemovePanelExtension(
                "orbit.inspector.providers"));
        static_cast<void>(
            editor_ui::RemoveShellBand("orbit.navigation"));
        static_cast<void>(
            editor_ui::RemoveShellBand("orbit.viewport-authoring"));

        // Some headless/model tests construct StudioViewportPanels without an
        // active EditorUi/ImGui context. The model remains usable there and
        // the live app attaches these surfaces through normal construction.
        attached_ = false;
    }
}

StudioExpansionShell::~StudioExpansionShell()
{
    SavePersistentStateIfChanged();

    if (!attached_)
    {
        return;
    }

    static_cast<void>(
        GlobalInspectorProviders().Remove(
            "orbit.viewport.diagnostics"));
    static_cast<void>(
        GlobalInspectorProviders().Remove(
            "orbit.terrain.active-tool"));
    static_cast<void>(
        GlobalInspectorProviders().Remove(
            "orbit.path.bezier"));
    static_cast<void>(
        GlobalInspectorProviders().Remove(
            "orbit.viewport.target"));
    static_cast<void>(
        editor_ui::RemovePanelExtension(
            "orbit.inspector.providers"));
    static_cast<void>(
        editor_ui::RemoveShellBand("orbit.navigation"));
    static_cast<void>(
        editor_ui::RemoveShellBand("orbit.viewport-authoring"));
}

bool StudioExpansionShell::DrawContributions(
    editor_ui::PanelContext& context,
    const StudioContributionSurface surface,
    const bool responsiveOverflow,
    const bool verticalList)
{
    if (owner_ == nullptr ||
        owner_->session_ == nullptr ||
        !owner_->session_->World().HasWorld())
    {
        return false;
    }

    auto& commands =
        owner_->session_->World().CommandRegistry();

    bool invokedSuccessfully = false;
    std::vector<editor_ui::ActionPresentation> actions;
    for (const auto& contribution :
         GlobalStudioUiContributions().Catalog(surface))
    {
        if (contribution.kind != StudioContributionKind::Command ||
            !contribution.command.IsValid())
        {
            continue;
        }

        const auto enablement =
            commands.Enablement(contribution.command);
        const auto command = contribution.command;

        actions.push_back({
            .label = contribution.label,
            .enabled = enablement.enabled,
            .disabledReason = enablement.reason,
            .invoke =
                [this, command, &invokedSuccessfully]
                {
                    if (owner_ == nullptr ||
                        owner_->session_ == nullptr ||
                        !owner_->session_->World().HasWorld())
                    {
                        return;
                    }

                    try
                    {
                        owner_->session_->World().CommandRegistry().Invoke(
                            command);
                        owner_->status_.clear();
                        invokedSuccessfully = true;
                    }
                    catch (const std::exception& exception)
                    {
                        owner_->status_ = exception.what();
                    }
                }
        });
    }

    if (actions.empty())
    {
        return false;
    }

    if (verticalList)
    {
        static_cast<void>(context.ActionList(actions));
        return invokedSuccessfully;
    }

    context.SameLine();

    // Keep built-in shell controls stable. Only extensible contributions
    // collapse when the current row runs short on horizontal space.
    // ContentAvailable() is evaluated after all high-priority controls have
    // drawn, so this adapts to both window width and the active context.
    constexpr f32 kInlineContributionReserve = 300.0F;
    const bool overflow =
        responsiveOverflow &&
        context.ContentAvailable().width <
            kInlineContributionReserve * editor_ui::CurrentUiScale();

    if (!overflow)
    {
        context.Toolbar(actions);
        return invokedSuccessfully;
    }

    const bool workspaceSurface =
        surface == StudioContributionSurface::WorkspaceToolbar;
    const char* overflowButton =
        workspaceSurface
            ? "…##studio-workspace-toolbar-overflow"
            : "…##studio-context-toolbar-overflow";
    const char* overflowMenu =
        workspaceSurface
            ? "studio-workspace-toolbar-overflow-menu"
            : "studio-context-toolbar-overflow-menu";

    const bool openOverflow =
        context.Button(overflowButton);
    context.ContextMenu(
        overflowMenu,
        actions,
        openOverflow);
    return invokedSuccessfully;
}

} // namespace orbit::studio_ui
