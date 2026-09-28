#include <orbit/studio_ui/StudioExpansionShell.hpp>

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

[[nodiscard]] const char* LayoutName(
    const ViewportLayout layout) noexcept
{
    switch (layout)
    {
    case ViewportLayout::Single: return "Single";
    case ViewportLayout::VerticalSplit: return "Vertical";
    case ViewportLayout::HorizontalSplit: return "Horizontal";
    case ViewportLayout::Quad: return "Quad";
    }
    return "Single";
}

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
        editor_ui::UpsertShellBand({
            .id = "orbit.navigation",
            .order = 15,
            .height = 40.0F,
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawNavigationBand(context);
                }
        });

        editor_ui::UpsertShellBand({
            .id = "orbit.viewport-authoring",
            .order = 20,
            .height = 40.0F,
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawViewportBand(context);
                }
        });

        attached_ = true;
    }
    catch (...)
    {
        // Some headless/model tests construct StudioViewportPanels without an
        // active EditorUi/ImGui context. The model remains usable there and
        // the live app attaches these bands through normal construction.
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

void StudioExpansionShell::DrawNavigationBand(
    editor_ui::PanelContext& context)
{
    SyncPersistentState();

    context.Text("Navigate");

    if (owner_ == nullptr ||
        owner_->session_ == nullptr ||
        !owner_->session_->World().HasWorld())
    {
        context.SameLine();
        context.MutedText("Open a world for breadcrumbs and commands.");
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

        const std::size_t first =
            breadcrumbs.size() > 4U
                ? breadcrumbs.size() - 4U
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

    context.SameLine();
    if (context.Button(
            quickCreateOpen_
                ? "Close Add##quick-create-toggle"
                : "+ Add##quick-create-toggle"))
    {
        quickCreateOpen_ = !quickCreateOpen_;
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

            if (++shown >= 3U)
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

            if (++shown >= 4U)
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

    context.Text("Viewport");
    context.SameLine();

    static constexpr std::array<std::string_view, 4>
        kLayouts{"Single", "Vertical", "Horizontal", "Quad"};
    i32 layout = static_cast<i32>(viewportState_.layout);
    if (context.SegmentedControl(
            "viewport-layout",
            kLayouts,
            layout))
    {
        viewportState_.SetLayout(
            static_cast<ViewportLayout>(layout));
    }

    context.SameLine();
    context.MutedText(LayoutName(viewportState_.layout));
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
