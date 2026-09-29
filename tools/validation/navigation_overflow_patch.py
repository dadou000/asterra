from pathlib import Path

path = Path('engine/studio_ui/src/StudioExpansionShell.cpp')
text = path.read_text(encoding='utf-8')
old = '''        // Keep enough ancestry to orient the user without letting a deep
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
'''
new = '''        // Breadcrumbs are the flexible part of row 1. Preserve enough room
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
'''
if old not in text:
    raise SystemExit('breadcrumb block anchor not found')
path.write_text(text.replace(old, new, 1), encoding='utf-8')
