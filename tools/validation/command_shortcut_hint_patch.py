from pathlib import Path

path = Path('engine/studio_ui/src/StudioExpansionShell.cpp')
text = path.read_text(encoding='utf-8')
old = '''    context.SameLine();
    const bool openCommandPalette =
        context.Button("Commands##command-palette-toggle") ||
        std::exchange(commandPaletteOpenRequested_, false);
'''
new = '''    context.SameLine();
    const f32 commandHintThreshold =
        230.0F * editor_ui::CurrentUiScale();
    const std::string_view commandButtonLabel =
        context.ContentAvailable().width >= commandHintThreshold
            ? "Commands  Ctrl+Shift+P##command-palette-toggle"
            : "Commands##command-palette-toggle";
    const bool openCommandPalette =
        context.Button(commandButtonLabel) ||
        std::exchange(commandPaletteOpenRequested_, false);
'''

count = text.count(old)
if count != 1:
    raise SystemExit(
        f'expected exactly one command palette button anchor, found {count}')

path.write_text(text.replace(old, new, 1), encoding='utf-8')
