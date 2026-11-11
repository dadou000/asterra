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

void StudioExpansionShell::DrawCommandPalettePopup(editor_ui::PanelContext& context, bool openCommandPalette,
        commands::CommandRegistry& registry,
        const std::vector<CommandPaletteEntry>& palette)
{
    const f32 commandPaletteWidth =
        520.0F * editor_ui::CurrentUiScale();
    if (context.BeginPopup(
            "studio-command-palette-popup",
            openCommandPalette,
            {.width = commandPaletteWidth, .height = 0.0F}))
    {
        context.Text("Commands");
        if (openCommandPalette)
        {
            context.FocusNextItem();
        }
        if (context.InputText(
                "##studio-command-palette-query",
                commandQuery_))
        {
            commandPaletteSelection_ = 0;
        }
        context.Separator();

        const auto matches =
            SearchCommandPalette(
                palette,
                commandQuery_,
                palette.size());

        std::vector<CommandPaletteEntry> visibleMatches;
        visibleMatches.reserve(8U);
        for (const auto& entry : matches)
        {
            if (entry.requiresArguments ||
                !registry.Enablement(entry.command).enabled)
            {
                continue;
            }

            visibleMatches.push_back(entry);
            if (visibleMatches.size() >= 8U)
            {
                break;
            }
        }

        const auto invokeEntry =
            [&](const CommandPaletteEntry& entry)
            {
                try
                {
                    registry.Invoke(entry.command);
                    owner_->status_.clear();
                    commandQuery_.clear();
                    commandPaletteSelection_ = 0;
                    context.CloseCurrentPopup();
                    return true;
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                    return false;
                }
            };

        if (context.KeyPressed(editor_ui::UiKey::Escape))
        {
            commandQuery_.clear();
            commandPaletteSelection_ = 0;
            context.CloseCurrentPopup();
        }
        else if (visibleMatches.empty())
        {
            commandPaletteSelection_ = 0;
            context.MutedText("No matching enabled commands.");
        }
        else
        {
            const i32 visibleCount =
                static_cast<i32>(visibleMatches.size());
            commandPaletteSelection_ = std::clamp(
                commandPaletteSelection_,
                0,
                visibleCount - 1);

            if (context.KeyPressed(
                    editor_ui::UiKey::Down,
                    true))
            {
                commandPaletteSelection_ =
                    (commandPaletteSelection_ + 1) % visibleCount;
            }
            if (context.KeyPressed(
                    editor_ui::UiKey::Up,
                    true))
            {
                commandPaletteSelection_ =
                    (commandPaletteSelection_ + visibleCount - 1) %
                    visibleCount;
            }

            bool invoked = false;
            for (i32 index = 0; index < visibleCount; ++index)
            {
                const auto& entry =
                    visibleMatches[static_cast<std::size_t>(index)];
                std::string label = entry.label;
                const std::string secondary =
                    CommandPaletteSecondaryText(entry);
                if (!secondary.empty())
                {
                    label += "\n";
                    label += secondary;
                }
                label += "##palette-";
                label += entry.command.ToString();
                if (context.Selectable(
                        label,
                        index == commandPaletteSelection_))
                {
                    commandPaletteSelection_ = index;
                    invoked = invokeEntry(entry);
                    if (invoked)
                    {
                        break;
                    }
                }
            }

            if (!invoked &&
                context.KeyPressed(editor_ui::UiKey::Enter))
            {
                static_cast<void>(
                    invokeEntry(
                        visibleMatches[static_cast<std::size_t>(
                            commandPaletteSelection_)]));
            }
        }

        context.EndPopup();
    }

}
} // namespace orbit::studio_ui
