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

bool StudioExpansionShell::DrawQuickCreatePopup(editor_ui::PanelContext& context, bool openQuickCreate,
        commands::CommandRegistry& registry,
        const std::vector<CommandPaletteEntry>& palette)
{
    auto& world = owner_->session_->World();
    bool openQuickCreateBrowser = false;
    const f32 quickCreateWidth =
        360.0F * editor_ui::CurrentUiScale();
    if (context.BeginPopup(
            "studio-quick-create-popup",
            openQuickCreate,
            {.width = quickCreateWidth, .height = 0.0F}))
    {
        context.Text("Add");

        bool closeQuickCreate = false;

        struct RankedQuickCreateAction
        {
            i32 priority{0};
            editor_ui::ActionPresentation action;
        };

        std::vector<RankedQuickCreateAction> rankedCommandActions;
        rankedCommandActions.reserve(8U);
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

            const i32 priority =
                owner_->QuickCreateCommandPriority(entry.category);
            if (priority <= 0)
            {
                continue;
            }

            const auto command = entry.command;
            std::string label = entry.label;
            label += "##quick-create-";
            label += command.ToString();
            rankedCommandActions.push_back({
                .priority = priority,
                .action = {
                    .label = std::move(label),
                    .invoke =
                        [&registry, &closeQuickCreate, command, this]
                        {
                            try
                            {
                                registry.Invoke(command);
                                owner_->status_.clear();
                                closeQuickCreate = true;
                            }
                            catch (const std::exception& exception)
                            {
                                owner_->status_ = exception.what();
                            }
                        }
                }
            });
        }

        std::stable_sort(
            rankedCommandActions.begin(),
            rankedCommandActions.end(),
            [](const RankedQuickCreateAction& left,
               const RankedQuickCreateAction& right)
            {
                return left.priority > right.priority;
            });

        std::vector<editor_ui::ActionPresentation> contextualCommandActions;
        std::vector<editor_ui::ActionPresentation> fallbackCommandActions;
        contextualCommandActions.reserve(4U);
        fallbackCommandActions.reserve(3U);

        for (auto& ranked : rankedCommandActions)
        {
            if (ranked.priority > 1)
            {
                if (contextualCommandActions.size() < 4U)
                {
                    contextualCommandActions.push_back(
                        std::move(ranked.action));
                }
            }
            else if (fallbackCommandActions.size() < 3U)
            {
                fallbackCommandActions.push_back(
                    std::move(ranked.action));
            }
        }

        bool drewQuickCreateSection = false;
        const auto drawCommandActions =
            [&](const std::string_view heading,
                const std::vector<editor_ui::ActionPresentation>& actions)
            {
                if (actions.empty())
                {
                    return;
                }

                if (drewQuickCreateSection)
                {
                    context.Separator();
                }
                context.MutedText(heading);
                static_cast<void>(context.ActionList(actions));
                drewQuickCreateSection = true;
            };

        const auto drawContextualCommands = [&]
        {
            drawCommandActions(
                world.Selection().Ordered().empty()
                    ? std::string_view{"For Workspace"}
                    : std::string_view{"For Selection"},
                contextualCommandActions);
        };

        const auto drawFallbackCommands = [&]
        {
            drawCommandActions(
                "More",
                fallbackCommandActions);
        };

        const auto drawViewportActions = [&]
        {
            if (!owner_->ShowViewportQuickCreate())
            {
                return;
            }

            if (drewQuickCreateSection)
            {
                context.Separator();
            }
            context.MutedText("Viewport");
            closeQuickCreate =
                DrawBuiltInQuickCreate(context) ||
                closeQuickCreate;
            drewQuickCreateSection = true;
        };

        const bool hasSelection =
            !world.Selection().Ordered().empty();
        if (owner_->PreferCommandQuickCreate() || hasSelection)
        {
            drawContextualCommands();
            drawViewportActions();
        }
        else
        {
            drawViewportActions();
            drawContextualCommands();
        }
        drawFallbackCommands();

        const auto pluginCatalog =
            GlobalStudioUiContributions().Catalog(
                StudioContributionSurface::QuickCreate);

        struct RankedPluginQuickCreateAction
        {
            i32 priority{0};
            editor_ui::ActionPresentation action;
        };

        std::vector<RankedPluginQuickCreateAction> rankedPluginActions;
        rankedPluginActions.reserve(pluginCatalog.size());

        for (const auto& contribution : pluginCatalog)
        {
            if (contribution.kind != StudioContributionKind::Command ||
                !contribution.command.IsValid())
            {
                continue;
            }

            const auto enablement =
                registry.Enablement(contribution.command);
            if (!enablement.enabled)
            {
                continue;
            }

            std::string_view category = contribution.category;
            if (category.empty())
            {
                const auto commandEntry =
                    std::ranges::find_if(
                        palette,
                        [&contribution](const CommandPaletteEntry& entry)
                        {
                            return entry.command == contribution.command;
                        });
                if (commandEntry != palette.end())
                {
                    category = commandEntry->category;
                }
            }

            const i32 priority =
                owner_->QuickCreateCommandPriority(category);
            if (priority <= 0)
            {
                continue;
            }

            const auto command = contribution.command;
            std::string label = contribution.label;
            label += "##quick-create-extension-";
            label += contribution.id;

            rankedPluginActions.push_back({
                .priority = priority,
                .action = {
                    .label = std::move(label),
                    .invoke =
                        [this, &closeQuickCreate, command]
                        {
                            try
                            {
                                owner_->session_->World().CommandRegistry().Invoke(
                                    command);
                                owner_->status_.clear();
                                closeQuickCreate = true;
                            }
                            catch (const std::exception& exception)
                            {
                                owner_->status_ = exception.what();
                            }
                        }
                }
            });
        }

        std::stable_sort(
            rankedPluginActions.begin(),
            rankedPluginActions.end(),
            [](const RankedPluginQuickCreateAction& left,
               const RankedPluginQuickCreateAction& right)
            {
                return left.priority > right.priority;
            });

        std::vector<editor_ui::ActionPresentation> pluginContextualActions;
        std::vector<editor_ui::ActionPresentation> pluginFallbackActions;
        pluginContextualActions.reserve(4U);
        pluginFallbackActions.reserve(3U);

        for (auto& ranked : rankedPluginActions)
        {
            if (ranked.priority > 1)
            {
                if (pluginContextualActions.size() < 4U)
                {
                    pluginContextualActions.push_back(
                        std::move(ranked.action));
                }
            }
            else if (pluginFallbackActions.size() < 3U)
            {
                pluginFallbackActions.push_back(
                    std::move(ranked.action));
            }
        }

        drawCommandActions(
            "Extensions",
            pluginContextualActions);
        drawCommandActions(
            "More Extensions",
            pluginFallbackActions);

        if (!drewQuickCreateSection)
        {
            context.MutedText(
                "No creation actions are relevant to the current workspace and selection.");
        }

        if (drewQuickCreateSection)
        {
            context.Separator();
        }
        if (context.Button("Browse All…##quick-create-browse-all"))
        {
            quickCreateBrowseQuery_.clear();
            quickCreateBrowseSelection_ = 0;
            openQuickCreateBrowser = true;
            closeQuickCreate = true;
        }

        if (closeQuickCreate)
        {
            context.CloseCurrentPopup();
        }

        context.EndPopup();
    }

    return openQuickCreateBrowser;

}
} // namespace orbit::studio_ui
