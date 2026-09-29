from pathlib import Path

path = Path('engine/studio_ui/src/StudioExpansionShell.cpp')
text = path.read_text(encoding='utf-8')

old = '''    // Keep built-in authoring/view/gizmo controls stable. Only extensible\n    // context commands collapse when the row runs short on horizontal space.\n    // ContentAvailable() is evaluated after all high-priority controls have\n    // drawn, so this adapts to both window width and the active context.\n    constexpr f32 kInlineContributionReserve = 300.0F;\n    const bool overflow =\n        responsiveOverflow &&\n        context.ContentAvailable().width <\n            kInlineContributionReserve * editor_ui::CurrentUiScale();\n\n    if (!overflow)\n    {\n        context.Toolbar(actions);\n        return invokedSuccessfully;\n    }\n\n    const bool openOverflow =\n        context.Button("…##studio-context-toolbar-overflow");\n    context.ContextMenu(\n        "studio-context-toolbar-overflow-menu",\n        actions,\n        openOverflow);\n    return invokedSuccessfully;\n'''
new = '''    // Keep built-in shell controls stable. Only extensible contributions\n    // collapse when the current row runs short on horizontal space.\n    // ContentAvailable() is evaluated after all high-priority controls have\n    // drawn, so this adapts to both window width and the active context.\n    constexpr f32 kInlineContributionReserve = 300.0F;\n    const bool overflow =\n        responsiveOverflow &&\n        context.ContentAvailable().width <\n            kInlineContributionReserve * editor_ui::CurrentUiScale();\n\n    if (!overflow)\n    {\n        context.Toolbar(actions);\n        return invokedSuccessfully;\n    }\n\n    const bool workspaceSurface =\n        surface == StudioContributionSurface::WorkspaceToolbar;\n    const char* overflowButton =\n        workspaceSurface\n            ? "…##studio-workspace-toolbar-overflow"\n            : "…##studio-context-toolbar-overflow";\n    const char* overflowMenu =\n        workspaceSurface\n            ? "studio-workspace-toolbar-overflow-menu"\n            : "studio-context-toolbar-overflow-menu";\n\n    const bool openOverflow =\n        context.Button(overflowButton);\n    context.ContextMenu(\n        overflowMenu,\n        actions,\n        openOverflow);\n    return invokedSuccessfully;\n'''
if old not in text:
    raise SystemExit('overflow block anchor not found')
text = text.replace(old, new, 1)

old = '''    static_cast<void>(DrawContributions(\n        context,\n        StudioContributionSurface::WorkspaceToolbar));\n'''
new = '''    static_cast<void>(DrawContributions(\n        context,\n        StudioContributionSurface::WorkspaceToolbar,\n        true));\n'''
if old not in text:
    raise SystemExit('workspace toolbar call anchor not found')
text = text.replace(old, new, 1)

path.write_text(text, encoding='utf-8')
