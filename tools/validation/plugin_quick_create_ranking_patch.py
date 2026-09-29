from pathlib import Path

path = Path('engine/studio_ui/src/StudioExpansionShell.cpp')
text = path.read_text(encoding='utf-8')

old = '''        const auto pluginCatalog =
            GlobalStudioUiContributions().Catalog(
                StudioContributionSurface::QuickCreate);
        const bool hasPluginQuickCreateCommands =
            std::ranges::any_of(
                pluginCatalog,
                [](const StudioUiContribution& contribution)
                {
                    return contribution.kind ==
                               StudioContributionKind::Command &&
                        contribution.command.IsValid();
                });
        if (hasPluginQuickCreateCommands)
        {
            if (drewQuickCreateSection)
            {
                context.Separator();
            }
            context.MutedText("Extensions");
            closeQuickCreate =
                DrawContributions(
                    context,
                    StudioContributionSurface::QuickCreate,
                    false,
                    true) ||
                closeQuickCreate;
            drewQuickCreateSection = true;
        }
'''

new = '''        const auto pluginCatalog =
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
'''

count = text.count(old)
if count != 1:
    raise SystemExit(f'expected exactly one plugin quick-create anchor, found {count}')

path.write_text(text.replace(old, new, 1), encoding='utf-8')
