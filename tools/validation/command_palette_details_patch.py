from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    file = Path(path)
    text = file.read_text(encoding='utf-8')
    if old not in text:
        raise SystemExit(f'anchor not found in {path}')
    file.write_text(text.replace(old, new, 1), encoding='utf-8')


replace_once(
    'engine/studio_ui/src/StudioExpansionShell.cpp',
    '''[[nodiscard]] bool IsQuickCreateLabel(\n    const std::string_view label) noexcept\n{\n    return label.starts_with("Add ") ||\n        label.starts_with("Create ") ||\n        label.starts_with("New ");\n}\n\n[[nodiscard]] bool IsTerrainSplineTool(\n''',
    '''[[nodiscard]] bool IsQuickCreateLabel(\n    const std::string_view label) noexcept\n{\n    return label.starts_with("Add ") ||\n        label.starts_with("Create ") ||\n        label.starts_with("New ");\n}\n\n[[nodiscard]] std::string CommandPaletteSecondaryText(\n    const CommandPaletteEntry& entry)\n{\n    std::string secondary = entry.category;\n    if (!entry.description.empty())\n    {\n        if (!secondary.empty())\n        {\n            secondary += " · ";\n        }\n        secondary += entry.description;\n    }\n\n    for (char& value : secondary)\n    {\n        if (value == '\\n' ||\n            value == '\\r' ||\n            value == '\\t')\n        {\n            value = ' ';\n        }\n    }\n\n    constexpr std::size_t kMaxSecondaryCharacters = 96U;\n    if (secondary.size() > kMaxSecondaryCharacters)\n    {\n        secondary.resize(kMaxSecondaryCharacters - 3U);\n        secondary += "...";\n    }\n\n    return secondary;\n}\n\n[[nodiscard]] bool IsTerrainSplineTool(\n''')

replace_once(
    'engine/studio_ui/src/StudioExpansionShell.cpp',
    '''                std::string label = entry.label;\n                label += "##palette-";\n                label += entry.command.ToString();\n                if (context.Selectable(\n''',
    '''                std::string label = entry.label;\n                const std::string secondary =\n                    CommandPaletteSecondaryText(entry);\n                if (!secondary.empty())\n                {\n                    label += "\\n";\n                    label += secondary;\n                }\n                label += "##palette-";\n                label += entry.command.ToString();\n                if (context.Selectable(\n''')
