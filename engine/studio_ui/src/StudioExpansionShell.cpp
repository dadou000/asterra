#include <orbit/studio_ui/StudioExpansionShell.hpp>

#include <orbit/editor_ui/PanelExtensions.hpp>
#include <orbit/studio_ui/CommandPaletteModel.hpp>
#include <orbit/studio_ui/SelectionBreadcrumbs.hpp>
#include <orbit/studio_ui/StudioViewportPanels.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orbit::studio_ui
{
namespace
{
InspectorProviderRegistry g_inspectorProviders;
StudioUiContributionRegistry g_uiContributions;

[[nodiscard]] bool IsQuickCreateLabel(
    const std::string_view label) noexcept
{
    return label.starts_with("Add ") ||
        label.starts_with("Create ") ||
        label.starts_with("New ");
}
} // namespace

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

        attached_ = true;
    }
    catch (...)
    {
        // Registration is transactional from the shell owner's point of view:
        // never leave callbacks retaining this object after a partial setup.
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
        editor_ui::RemovePanelExtension(
            "orbit.inspector.providers"));
    static_cast<void>(
        editor_ui::RemoveShellBand("orbit.navigation"));
    static_cast<void>(
        editor_ui::RemoveShellBand("orbit.viewport-authoring"));
}

void StudioExpansionShell::SavePersistentStateIfChanged() noexcept
{
    if (!persistentStateLoaded_ ||
        persistentStatePath_.empty())
    {
        return;
    }

    persistentState_.viewport = viewportState_;
    if (owner_ != nullptr)
    {
        persistentState_.inspectorAdvanced =
            owner_->contextualAdvancedProperties_;
    }

    const std::string serialized =
        SerializeStudioPersistentState(
            persistentState_);

    if (serialized == persistentSnapshot_)
    {
        return;
    }

    try
    {
        SaveStudioPersistentState(
            persistentStatePath_,
            persistentState_);
        persistentSnapshot_ = serialized;
    }
    catch (const std::exception& exception)
    {
        if (owner_ != nullptr)
        {
            owner_->status_ =
                std::string{"Studio state save failed: "} +
                exception.what();
        }
    }
}

void StudioExpansionShell::SyncPersistentState() noexcept
{
    studio_session::StudioSession* const session =
        owner_ != nullptr
            ? owner_->session_
            : nullptr;

    if (session == persistentSession_ &&
        persistentStateLoaded_)
    {
        SavePersistentStateIfChanged();
        return;
    }

    // Rebinding projects is a normal Studio operation. Persist the old
    // project before swapping the presentation binding.
    SavePersistentStateIfChanged();

    persistentSession_ = session;
    persistentStatePath_.clear();
    persistentState_ = {};
    persistentSnapshot_.clear();
    persistentStateLoaded_ = false;

    if (session == nullptr)
    {
        return;
    }

    try
    {
        persistentStatePath_ =
            session->World().Project().RootDirectory() /
            ".orbit" /
            "StudioState.ini";

        persistentState_ =
            LoadStudioPersistentState(
                persistentStatePath_);

        viewportState_ = persistentState_.viewport;
        if (owner_ != nullptr)
        {
            owner_->contextualAdvancedProperties_ =
                persistentState_.inspectorAdvanced;
        }

        persistentSnapshot_ =
            SerializeStudioPersistentState(
                persistentState_);
        persistentStateLoaded_ = true;
    }
    catch (const std::exception& exception)
    {
        if (owner_ != nullptr)
        {
            owner_->status_ =
                std::string{"Studio state load failed: "} +
                exception.what();
        }
    }
}

void StudioExpansionShell::DrawContributions(
    editor_ui::PanelContext& context,
    const StudioContributionSurface surface)
{
    if (owner_ == nullptr ||
        owner_->session_ == nullptr ||
        !owner_->session_->World().HasWorld())
    {
        return;
    }

    auto& commands =
        owner_->session_->World().CommandRegistry();

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
                [this, command]
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
                    }
                    catch (const std::exception& exception)
                    {
                        owner_->status_ = exception.what();
                    }
                }
        });
    }

    if (!actions.empty())
    {
        context.SameLine();
        context.Toolbar(actions);
    }
}

void StudioExpansionShell::DrawInspectorExtension(
    editor_ui::PanelContext& context)
{
    SyncPersistentState();

    const auto providers =
        GlobalInspectorProviders().Relevant();

    if (providers.empty())
    {
        return;
    }

    context.Separator();
    context.MutedText("Extensions");
    static_cast<void>(
        GlobalInspectorProviders().DrawRelevant(
            context));
}

void StudioExpansionShell::DrawNavigationBand(
    editor_ui::PanelContext& context)
{
    SyncPersistentState();

    // Workspace selection and navigation are one mental model. The owner draws
    // the mode selector first; breadcrumbs, quick-create and command search
    // continue on the same row instead of reserving another strip of viewport.
    if (owner_ != nullptr)
    {
        owner_->DrawWorkspaceBand(context);
    }
    else
    {
        context.Text("Mode");
    }

    context.SameLine();
    context.MutedText("|");

    if (owner_ == nullptr ||
        owner_->session_ == nullptr ||
        !owner_->session_->World().HasWorld())
    {
        context.SameLine();
        context.MutedText("Open a world for navigation and commands.");
        return;
    }

    auto& world = owner_->session_->World();
    const auto& selection = world.Selection().Ordered();

    if (selection.size() == 1U)
    {
        const auto breadcrumbs =
            BuildSelectionBreadcrumbs(
                world.Objects(),
                selection.front());

        // Keep enough ancestry to orient the user without letting a deep
        // hierarchy consume the entire permanent row.
        const std::size_t first =
            breadcrumbs.size() > 3U
                ? breadcrumbs.size() - 3U
                : 0U;

        for (std::size_t index = first;
             index < breadcrumbs.size();
             ++index)
        {
            context.SameLine();
            if (index != first)
            {
                context.MutedText(">");
                context.SameLine();
            }

            const auto& breadcrumb = breadcrumbs[index];
            std::string label = breadcrumb.label;
            label += "##breadcrumb-";
            label += breadcrumb.id.ToString();

            if (context.Button(label))
            {
                const std::array selected{breadcrumb.id};
                world.Selection().Set(
                    std::span<const scene::ObjectId>(selected));
            }
        }
    }

    context.SameLine();
    if (context.Button(
            quickCreateOpen_
                ? "Close Add##quick-create-toggle"
                : "+ Add##quick-create-toggle"))
    {
        quickCreateOpen_ = !quickCreateOpen_;
    }

    context.SameLine();
    if (context.Button(
            commandSearchOpen_
                ? "Close Commands##command-palette-toggle"
                : "Commands##command-palette-toggle"))
    {
        commandSearchOpen_ = !commandSearchOpen_;
        if (!commandSearchOpen_)
        {
            commandQuery_.clear();
        }
    }

    auto& registry = world.CommandRegistry();
    const auto palette =
        BuildCommandPalette(registry.Catalog());

    if (commandSearchOpen_)
    {
        context.SameLine();
        static_cast<void>(
            context.InputText(
                "##studio-command-palette-query",
                commandQuery_));

        const auto matches =
            SearchCommandPalette(
                palette,
                commandQuery_,
                8U);

        std::size_t shown = 0U;
        for (const auto& entry : matches)
        {
            if (entry.requiresArguments)
            {
                continue;
            }

            const auto enablement =
                registry.Enablement(entry.command);
            if (!enablement.enabled)
            {
                continue;
            }

            context.SameLine();
            std::string label = entry.label;
            label += "##palette-";
            label += entry.command.ToString();
            if (context.Button(label))
            {
                try
                {
                    registry.Invoke(entry.command);
                    owner_->status_.clear();
                    commandSearchOpen_ = false;
                    commandQuery_.clear();
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }

            if (++shown >= 2U)
            {
                break;
            }
        }
    }

    if (quickCreateOpen_)
    {
        std::size_t shown = 0U;
        for (const auto& entry : palette)
        {
            if (entry.requiresArguments ||
                !IsQuickCreateLabel(entry.label))
            {
                continue;
            }

            const auto enablement =
                registry.Enablement(entry.command);
            if (!enablement.enabled)
            {
                continue;
            }

            context.SameLine();
            std::string label = entry.label;
            label += "##quick-create-";
            label += entry.command.ToString();
            if (context.Button(label))
            {
                try
                {
                    registry.Invoke(entry.command);
                    owner_->status_.clear();
                    quickCreateOpen_ = false;
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }

            if (++shown >= 3U)
            {
                break;
            }
        }

        DrawContributions(
            context,
            StudioContributionSurface::QuickCreate);
    }

    DrawContributions(
        context,
        StudioContributionSurface::WorkspaceToolbar);
}

void StudioExpansionShell::DrawViewportBand(
    editor_ui::PanelContext& context)
{
    SyncPersistentState();

    // Selection-driven authoring is the primary content of row two. Generic
    // viewport/gizmo state follows it compactly rather than living in a fourth
    // permanent toolbar.
    if (owner_ != nullptr)
    {
        owner_->DrawContextBand(context);
    }
    else
    {
        context.Text("Context");
    }

    context.SameLine();
    context.MutedText("|");
    context.SameLine();
    context.Text("View");
    context.SameLine();

    static constexpr std::array<std::string_view, 4>
        kLayouts{"Single", "Vertical", "Horizontal", "Quad"};
    i32 layout = static_cast<i32>(viewportState_.layout);
    if (context.Combo(
            "##viewport-layout",
            kLayouts,
            layout))
    {
        viewportState_.SetLayout(
            static_cast<ViewportLayout>(layout));
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

    context.SameLine();
    if (context.Button(
            viewportState_.gizmo.space == GizmoSpace::World
                ? "World##gizmo-space"
                : "Local##gizmo-space"))
    {
        viewportState_.gizmo.space =
            viewportState_.gizmo.space == GizmoSpace::World
                ? GizmoSpace::Local
                : GizmoSpace::World;
    }

    context.SameLine();
    if (context.Button(
            viewportState_.gizmo.translationSnap
                ? "Snap On##gizmo-snap"
                : "Snap Off##gizmo-snap"))
    {
        viewportState_.gizmo.translationSnap =
            !viewportState_.gizmo.translationSnap;
    }

    context.SameLine();
    if (context.Button(
            viewportState_.gizmo.surfaceSnap
                ? "Surface On##gizmo-surface"
                : "Surface Off##gizmo-surface"))
    {
        viewportState_.gizmo.surfaceSnap =
            !viewportState_.gizmo.surfaceSnap;
    }

    DrawContributions(
        context,
        StudioContributionSurface::ContextToolbar);
}
} // namespace orbit::studio_ui
