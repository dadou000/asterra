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

        // Breadcrumbs are the flexible part of row 1. Preserve enough room
        // for the two high-frequency actions that follow (+ Add and Commands),
        // then retain the selected object and as many nearest parents as fit.
        constexpr std::size_t kMaximumVisibleBreadcrumbs = 3U;
        const f32 scale = editor_ui::CurrentUiScale();
        const f32 breadcrumbBudget = std::max(
            0.0F,
            context.ContentAvailable().width - 300.0F * scale);

        const auto estimatedWidth =
            [scale, &breadcrumbs](
                const std::size_t first) noexcept
            {
                f32 width = 0.0F;
                for (std::size_t index = first;
                     index < breadcrumbs.size();
                     ++index)
                {
                    width +=
                        static_cast<f32>(breadcrumbs[index].label.size()) *
                            8.0F * scale +
                        30.0F * scale;
                    if (index != first)
                    {
                        width += 22.0F * scale;
                    }
                }
                return width;
            };

        std::size_t first =
            breadcrumbs.size() > kMaximumVisibleBreadcrumbs
                ? breadcrumbs.size() - kMaximumVisibleBreadcrumbs
                : 0U;
        while (first + 1U < breadcrumbs.size() &&
               estimatedWidth(first) > breadcrumbBudget)
        {
            ++first;
        }

        if (first > 0U)
        {
            std::vector<editor_ui::ActionPresentation> hiddenAncestors;
            hiddenAncestors.reserve(first);
            for (std::size_t index = 0U; index < first; ++index)
            {
                const auto breadcrumb = breadcrumbs[index];
                hiddenAncestors.push_back({
                    .label = breadcrumb.label,
                    .invoke =
                        [&world, id = breadcrumb.id]
                        {
                            const std::array selected{id};
                            world.Selection().Set(
                                std::span<const scene::ObjectId>(selected));
                        }
                });
            }

            context.SameLine();
            const bool openHidden =
                context.Button("…##breadcrumb-overflow");
            context.ContextMenu(
                "breadcrumb-overflow-menu",
                hiddenAncestors,
                openHidden);
        }

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
    const bool openQuickCreate =
        context.Button("+ Add##quick-create-toggle");

    context.SameLine();
    const f32 commandHintThreshold =
        230.0F * editor_ui::CurrentUiScale();
    const std::string_view commandButtonLabel =
        context.ContentAvailable().width >= commandHintThreshold
            ? "Commands  /##command-palette-toggle"
            : "Commands##command-palette-toggle";
    const bool openCommandPalette =
        context.Button(commandButtonLabel) ||
        std::exchange(commandPaletteOpenRequested_, false);
    if (openCommandPalette)
    {
        commandQuery_.clear();
        commandPaletteSelection_ = 0;
    }

    auto& registry = world.CommandRegistry();
    const auto commandCatalog = registry.Catalog();
    const auto palette =
        BuildCommandPalette(commandCatalog);

    DrawCommandPalettePopup(context, openCommandPalette, registry, palette);
    const bool openQuickCreateBrowser =
        DrawQuickCreatePopup(context, openQuickCreate, registry, palette);
    DrawQuickCreateBrowser(context, openQuickCreateBrowser, commandCatalog, registry, palette);

    static_cast<void>(DrawContributions(
        context,
        StudioContributionSurface::WorkspaceToolbar,
        true));
}
} // namespace orbit::studio_ui
