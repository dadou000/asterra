from pathlib import Path

path = Path('engine/studio_ui/src/StudioViewportPanels.cpp')
text = path.read_text(encoding='utf-8')
old = '''    context.Text("Mode");
    context.SameLine();

    const auto button =
        [&context](
            const char* name,
            const WorkspaceMode mode)
        {
            std::string label =
                g_workspaceMode == mode
                    ? std::string{"["} + name + "]"
                    : std::string{name};
            label += "##workspace-strip-";
            label += name;

            if (context.Button(label))
            {
                ActivateWorkspace(*g_workspaceUi, mode);
            }
        };

    button("Scene", WorkspaceMode::Scene);
    context.SameLine();
    button("Planet", WorkspaceMode::Planet);
    context.SameLine();
    button("Celestial", WorkspaceMode::Celestial);
    context.SameLine();
    button("Simulation", WorkspaceMode::Simulation);
    context.SameLine();
    button("Shading", WorkspaceMode::Shading);
'''
new = '''    context.Text("Mode");
    context.SameLine();

    static constexpr std::array<std::string_view, 5> kWorkspaceModes{
        "Scene",
        "Planet",
        "Celestial",
        "Simulation",
        "Shading"
    };

    i32 workspace = static_cast<i32>(g_workspaceMode);
    if (context.Combo(
            "##workspace-mode-compact",
            kWorkspaceModes,
            workspace))
    {
        workspace = std::clamp(workspace, 0, 4);
        ActivateWorkspace(
            *g_workspaceUi,
            static_cast<WorkspaceMode>(workspace));
    }
'''
if old not in text:
    raise SystemExit('workspace band anchor not found')
path.write_text(text.replace(old, new, 1), encoding='utf-8')
