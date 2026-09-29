from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    file = Path(path)
    text = file.read_text(encoding='utf-8')
    if old not in text:
        raise SystemExit(f'anchor not found in {path}')
    file.write_text(text.replace(old, new, 1), encoding='utf-8')


replace_once(
    'engine/editor_ui/include/orbit/editor_ui/EditorUi.hpp',
    '''    void Toolbar(std::span<const ActionPresentation> actions);\n    // Generic popup surface for rich transient UI such as command palettes.\n''',
    '''    void Toolbar(std::span<const ActionPresentation> actions);\n    // Vertical action surface with the same enablement and disabled-reason\n    // behavior as Toolbar. Returns true when an enabled action was invoked.\n    [[nodiscard]] bool ActionList(\n        std::span<const ActionPresentation> actions);\n    // Generic popup surface for rich transient UI such as command palettes.\n''')

replace_once(
    'engine/editor_ui/src/EditorUi.cpp',
    '''bool PanelContext::BeginPopup(\n    const std::string_view id,\n''',
    '''bool PanelContext::ActionList(\n    const std::span<const ActionPresentation> actions)\n{\n    bool invoked = false;\n\n    for (const ActionPresentation& action : actions)\n    {\n        if (!action.enabled)\n        {\n            ImGui::BeginDisabled();\n        }\n\n        const bool selected =\n            ImGui::Selectable(\n                action.label.c_str(),\n                false);\n\n        const bool hovered =\n            ImGui::IsItemHovered(\n                ImGuiHoveredFlags_AllowWhenDisabled);\n\n        if (!action.enabled)\n        {\n            ImGui::EndDisabled();\n\n            if (hovered &&\n                !action.disabledReason.empty())\n            {\n                ImGui::SetTooltip(\n                    "%s",\n                    action.disabledReason.c_str());\n            }\n        }\n\n        if (selected &&\n            action.enabled &&\n            action.invoke)\n        {\n            action.invoke();\n            invoked = true;\n        }\n    }\n\n    return invoked;\n}\n\nbool PanelContext::BeginPopup(\n    const std::string_view id,\n''')

replace_once(
    'engine/studio_ui/include/orbit/studio_ui/StudioExpansionShell.hpp',
    '''    void DrawContributions(\n        editor_ui::PanelContext& context,\n        StudioContributionSurface surface,\n        bool responsiveOverflow = false);\n''',
    '''    [[nodiscard]] bool DrawContributions(\n        editor_ui::PanelContext& context,\n        StudioContributionSurface surface,\n        bool responsiveOverflow = false,\n        bool verticalList = false);\n''')

replace_once(
    'engine/studio_ui/include/orbit/studio_ui/StudioExpansionShell.hpp',
    '''    [[nodiscard]] bool BezierContextRelevant() const noexcept;\n    void DrawBuiltInQuickCreate(editor_ui::PanelContext& context);\n''',
    '''    [[nodiscard]] bool BezierContextRelevant() const noexcept;\n    [[nodiscard]] bool DrawBuiltInQuickCreate(\n        editor_ui::PanelContext& context);\n''')

replace_once(
    'engine/studio_ui/include/orbit/studio_ui/StudioExpansionShell.hpp',
    '''    ViewportAuthoringState viewportState_{};\n    std::string commandQuery_;\n    bool quickCreateOpen_{false};\n    bool attached_{false};\n''',
    '''    ViewportAuthoringState viewportState_{};\n    std::string commandQuery_;\n    bool attached_{false};\n''')

replace_once(
    'engine/studio_ui/src/StudioExpansionShell.cpp',
    '''void StudioExpansionShell::DrawContributions(\n    editor_ui::PanelContext& context,\n    const StudioContributionSurface surface,\n    const bool responsiveOverflow)\n{\n''',
    '''bool StudioExpansionShell::DrawContributions(\n    editor_ui::PanelContext& context,\n    const StudioContributionSurface surface,\n    const bool responsiveOverflow,\n    const bool verticalList)\n{\n''')

replace_once(
    'engine/studio_ui/src/StudioExpansionShell.cpp',
    '''    {\n        return;\n    }\n\n    auto& commands =\n        owner_->session_->World().CommandRegistry();\n\n    std::vector<editor_ui::ActionPresentation> actions;\n''',
    '''    {\n        return false;\n    }\n\n    auto& commands =\n        owner_->session_->World().CommandRegistry();\n\n    bool invokedSuccessfully = false;\n    std::vector<editor_ui::ActionPresentation> actions;\n''')

replace_once(
    'engine/studio_ui/src/StudioExpansionShell.cpp',
    '''            .invoke =\n                [this, command]\n                {\n''',
    '''            .invoke =\n                [this, command, &invokedSuccessfully]\n                {\n''')

replace_once(
    'engine/studio_ui/src/StudioExpansionShell.cpp',
    '''                        owner_->session_->World().CommandRegistry().Invoke(\n                            command);\n                        owner_->status_.clear();\n''',
    '''                        owner_->session_->World().CommandRegistry().Invoke(\n                            command);\n                        owner_->status_.clear();\n                        invokedSuccessfully = true;\n''')

replace_once(
    'engine/studio_ui/src/StudioExpansionShell.cpp',
    '''    if (actions.empty())\n    {\n        return;\n    }\n\n    context.SameLine();\n''',
    '''    if (actions.empty())\n    {\n        return false;\n    }\n\n    if (verticalList)\n    {\n        static_cast<void>(context.ActionList(actions));\n        return invokedSuccessfully;\n    }\n\n    context.SameLine();\n''')

replace_once(
    'engine/studio_ui/src/StudioExpansionShell.cpp',
    '''    if (!overflow)\n    {\n        context.Toolbar(actions);\n        return;\n    }\n''',
    '''    if (!overflow)\n    {\n        context.Toolbar(actions);\n        return invokedSuccessfully;\n    }\n''')

replace_once(
    'engine/studio_ui/src/StudioExpansionShell.cpp',
    '''    context.ContextMenu(\n        "studio-context-toolbar-overflow-menu",\n        actions,\n        openOverflow);\n}\n''',
    '''    context.ContextMenu(\n        "studio-context-toolbar-overflow-menu",\n        actions,\n        openOverflow);\n    return invokedSuccessfully;\n}\n''')

replace_once(
    'engine/studio_ui/src/StudioExpansionShell.cpp',
    '''void StudioExpansionShell::DrawBuiltInQuickCreate(\n    editor_ui::PanelContext& context)\n{\n    if (owner_ == nullptr)\n    {\n        return;\n    }\n''',
    '''bool StudioExpansionShell::DrawBuiltInQuickCreate(\n    editor_ui::PanelContext& context)\n{\n    if (owner_ == nullptr)\n    {\n        return false;\n    }\n\n    bool created = false;\n''')

for call in [
    '''                    owner_->CreateLocalLightAtViewport(\n                        viewportId,\n                        false);\n                    quickCreateOpen_ = false;\n''',
    '''                    owner_->CreateLocalLightAtViewport(\n                        viewportId,\n                        true);\n                    quickCreateOpen_ = false;\n''',
    '''                    owner_->CreateVisibilityProxyAtViewport(\n                        viewportId,\n                        false);\n                    quickCreateOpen_ = false;\n''',
    '''                    owner_->CreateVisibilityProxyAtViewport(\n                        viewportId,\n                        true);\n                    quickCreateOpen_ = false;\n''']:
    replacement = call.replace('                    quickCreateOpen_ = false;\n', '                    created = true;\n')
    text_path = Path('engine/studio_ui/src/StudioExpansionShell.cpp')
    text = text_path.read_text(encoding='utf-8')
    if call not in text:
        raise SystemExit('quick create action anchor not found')
    text_path.write_text(text.replace(call, replacement, 1), encoding='utf-8')

replace_once(
    'engine/studio_ui/src/StudioExpansionShell.cpp',
    '''            [this, viewportId]\n            {\n''',
    '''            [this, viewportId, &created]\n            {\n''')
# Apply the same capture update to the remaining three built-in actions.
for _ in range(3):
    replace_once(
        'engine/studio_ui/src/StudioExpansionShell.cpp',
        '''            [this, viewportId]\n            {\n''',
        '''            [this, viewportId, &created]\n            {\n''')

replace_once(
    'engine/studio_ui/src/StudioExpansionShell.cpp',
    '''    context.SameLine();\n    context.Toolbar(actions);\n}\n\nvoid StudioExpansionShell::DrawViewportTargetProperties(\n''',
    '''    static_cast<void>(context.ActionList(actions));\n    return created;\n}\n\nvoid StudioExpansionShell::DrawViewportTargetProperties(\n''')

replace_once(
    'engine/studio_ui/src/StudioExpansionShell.cpp',
    '''    context.SameLine();\n    if (context.Button(\n            quickCreateOpen_\n                ? "Close Add##quick-create-toggle"\n                : "+ Add##quick-create-toggle"))\n    {\n        quickCreateOpen_ = !quickCreateOpen_;\n    }\n\n    context.SameLine();\n''',
    '''    context.SameLine();\n    const bool openQuickCreate =\n        context.Button("+ Add##quick-create-toggle");\n\n    context.SameLine();\n''')

old_quick = '''    if (quickCreateOpen_)\n    {\n        DrawBuiltInQuickCreate(context);\n\n        std::size_t shown = 0U;\n        for (const auto& entry : palette)\n        {\n            if (entry.requiresArguments ||\n                !IsQuickCreateLabel(entry.label))\n            {\n                continue;\n            }\n\n            const auto enablement =\n                registry.Enablement(entry.command);\n            if (!enablement.enabled)\n            {\n                continue;\n            }\n\n            context.SameLine();\n            std::string label = entry.label;\n            label += "##quick-create-";\n            label += entry.command.ToString();\n            if (context.Button(label))\n            {\n                try\n                {\n                    registry.Invoke(entry.command);\n                    owner_->status_.clear();\n                    quickCreateOpen_ = false;\n                }\n                catch (const std::exception& exception)\n                {\n                    owner_->status_ = exception.what();\n                }\n            }\n\n            if (++shown >= 2U)\n            {\n                break;\n            }\n        }\n\n        DrawContributions(\n            context,\n            StudioContributionSurface::QuickCreate);\n    }\n'''
new_quick = '''    const f32 quickCreateWidth =\n        360.0F * editor_ui::CurrentUiScale();\n    if (context.BeginPopup(\n            "studio-quick-create-popup",\n            openQuickCreate,\n            {.width = quickCreateWidth, .height = 0.0F}))\n    {\n        context.Text("Add");\n\n        bool closeQuickCreate =\n            DrawBuiltInQuickCreate(context);\n\n        std::vector<editor_ui::ActionPresentation> commandActions;\n        commandActions.reserve(4U);\n        for (const auto& entry : palette)\n        {\n            if (entry.requiresArguments ||\n                !IsQuickCreateLabel(entry.label))\n            {\n                continue;\n            }\n\n            const auto enablement =\n                registry.Enablement(entry.command);\n            if (!enablement.enabled)\n            {\n                continue;\n            }\n\n            const auto command = entry.command;\n            std::string label = entry.label;\n            label += "##quick-create-";\n            label += command.ToString();\n            commandActions.push_back({\n                .label = std::move(label),\n                .invoke =\n                    [&registry, &closeQuickCreate, command, this]\n                    {\n                        try\n                        {\n                            registry.Invoke(command);\n                            owner_->status_.clear();\n                            closeQuickCreate = true;\n                        }\n                        catch (const std::exception& exception)\n                        {\n                            owner_->status_ = exception.what();\n                        }\n                    }\n            });\n\n            if (commandActions.size() >= 4U)\n            {\n                break;\n            }\n        }\n\n        if (!commandActions.empty())\n        {\n            context.Separator();\n            context.MutedText("Commands");\n            static_cast<void>(context.ActionList(commandActions));\n        }\n\n        const auto pluginCatalog =\n            GlobalStudioUiContributions().Catalog(\n                StudioContributionSurface::QuickCreate);\n        if (!pluginCatalog.empty())\n        {\n            context.Separator();\n            context.MutedText("Extensions");\n            closeQuickCreate =\n                DrawContributions(\n                    context,\n                    StudioContributionSurface::QuickCreate,\n                    false,\n                    true) ||\n                closeQuickCreate;\n        }\n\n        if (closeQuickCreate)\n        {\n            context.CloseCurrentPopup();\n        }\n\n        context.EndPopup();\n    }\n'''
replace_once(
    'engine/studio_ui/src/StudioExpansionShell.cpp',
    old_quick,
    new_quick)

# Existing non-popup callers intentionally ignore the success return value.
text_path = Path('engine/studio_ui/src/StudioExpansionShell.cpp')
text = text_path.read_text(encoding='utf-8')
text = text.replace(
    '''    DrawContributions(\n        context,\n        StudioContributionSurface::WorkspaceToolbar);\n''',
    '''    static_cast<void>(DrawContributions(\n        context,\n        StudioContributionSurface::WorkspaceToolbar));\n''')
text = text.replace(
    '''    DrawContributions(\n        context,\n        StudioContributionSurface::ContextToolbar,\n        true);\n''',
    '''    static_cast<void>(DrawContributions(\n        context,\n        StudioContributionSurface::ContextToolbar,\n        true));\n''')
text_path.write_text(text, encoding='utf-8')
