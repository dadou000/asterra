from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    file = Path(path)
    text = file.read_text(encoding='utf-8')
    if old not in text:
        raise SystemExit(f'anchor not found in {path}')
    file.write_text(text.replace(old, new, 1), encoding='utf-8')


# P is a logical/mnemonic shortcut key, just like Y/Z.
replace_once(
    'engine/platform/include/orbit/platform/Window.hpp',
    '''//  - C G L M V X Y Z (and LetterA) name the *letter* that key types on the\n''',
    '''//  - C G L M P V X Y Z (and LetterA) name the *letter* that key types on the\n''')
replace_once(
    'engine/platform/include/orbit/platform/Window.hpp',
    '''    L,\n    M,\n    V,\n''',
    '''    L,\n    M,\n    P,\n    V,\n''')
replace_once(
    'engine/platform/src/windows/Win32Keyboard.cpp',
    '''    case Key::M:\n        return L'M';\n    case Key::V:\n''',
    '''    case Key::M:\n        return L'M';\n    case Key::P:\n        return L'P';\n    case Key::V:\n''')

# ShortcutRegistry gets a generic callback binding. Global UI callbacks can opt
# out of the normal WantsKeyboard suppression; command shortcuts keep existing
# behavior by default.
replace_once(
    'engine/editor_model/include/orbit/editor_model/ShortcutRegistry.hpp',
    '''#include <orbit/platform/Window.hpp>\n\n#include <vector>\n''',
    '''#include <orbit/platform/Window.hpp>\n\n#include <functional>\n#include <vector>\n''')
replace_once(
    'engine/editor_model/include/orbit/editor_model/ShortcutRegistry.hpp',
    '''    void Register(\n        ShortcutChord chord,\n        commands::CommandId command);\n\n    void Update(\n''',
    '''    void Register(\n        ShortcutChord chord,\n        commands::CommandId command);\n\n    void RegisterCallback(\n        ShortcutChord chord,\n        std::function<void()> callback,\n        bool allowWhenKeyboardCaptured = false);\n\n    void Update(\n''')
replace_once(
    'engine/editor_model/include/orbit/editor_model/ShortcutRegistry.hpp',
    '''        ShortcutChord chord;\n        commands::CommandId command{};\n        bool wasDown{false};\n''',
    '''        ShortcutChord chord;\n        commands::CommandId command{};\n        std::function<void()> callback;\n        bool allowWhenKeyboardCaptured{false};\n        bool wasDown{false};\n''')

replace_once(
    'engine/editor_model/src/ShortcutRegistry.cpp',
    '''void ShortcutRegistry::Update(\n''',
    '''void ShortcutRegistry::RegisterCallback(\n    const ShortcutChord chord,\n    std::function<void()> callback,\n    const bool allowWhenKeyboardCaptured)\n{\n    if (!callback)\n    {\n        throw std::invalid_argument(\n            "Shortcut requires a callback.");\n    }\n\n    for (const Binding& binding :\n         bindings_)\n    {\n        if (binding.chord == chord)\n        {\n            throw std::invalid_argument(\n                "Shortcut chord is already registered.");\n        }\n    }\n\n    bindings_.push_back({\n        .chord = chord,\n        .callback = std::move(callback),\n        .allowWhenKeyboardCaptured = allowWhenKeyboardCaptured\n    });\n}\n\nvoid ShortcutRegistry::Update(\n''')
replace_once(
    'engine/editor_model/src/ShortcutRegistry.cpp',
    '''#include <stdexcept>\n''',
    '''#include <stdexcept>\n#include <utility>\n''')
replace_once(
    'engine/editor_model/src/ShortcutRegistry.cpp',
    '''        if (!suppressInvocation &&\n            down &&\n            !binding.wasDown)\n        {\n            const auto enablement =\n                commandRegistry.Enablement(\n                    binding.command);\n\n            if (enablement.enabled)\n            {\n                commandRegistry.Invoke(\n                    binding.command);\n            }\n        }\n''',
    '''        if ((!suppressInvocation ||\n             binding.allowWhenKeyboardCaptured) &&\n            down &&\n            !binding.wasDown)\n        {\n            if (binding.callback)\n            {\n                binding.callback();\n            }\n            else\n            {\n                const auto enablement =\n                    commandRegistry.Enablement(\n                        binding.command);\n\n                if (enablement.enabled)\n                {\n                    commandRegistry.Invoke(\n                        binding.command);\n                }\n            }\n        }\n''')

# Expose a tiny UI request bridge. The next navigation-band draw consumes the
# request and runs the exact same reset/focus/open path as the Commands button.
replace_once(
    'engine/studio_ui/include/orbit/studio_ui/StudioExpansionShell.hpp',
    '''    [[nodiscard]] std::string_view\n    ControlledViewportId() const noexcept\n    {\n        return SelectedViewportId();\n    }\n\nprivate:\n''',
    '''    [[nodiscard]] std::string_view\n    ControlledViewportId() const noexcept\n    {\n        return SelectedViewportId();\n    }\n\n    void RequestCommandPaletteOpen() noexcept\n    {\n        commandPaletteOpenRequested_ = true;\n    }\n\nprivate:\n''')
replace_once(
    'engine/studio_ui/include/orbit/studio_ui/StudioExpansionShell.hpp',
    '''    std::string commandQuery_;\n    i32 commandPaletteSelection_{0};\n    bool attached_{false};\n''',
    '''    std::string commandQuery_;\n    i32 commandPaletteSelection_{0};\n    bool commandPaletteOpenRequested_{false};\n    bool attached_{false};\n''')
replace_once(
    'engine/studio_ui/src/StudioExpansionShell.cpp',
    '''    const bool openCommandPalette =\n        context.Button("Commands##command-palette-toggle");\n''',
    '''    const bool openCommandPalette =\n        context.Button("Commands##command-palette-toggle") ||\n        std::exchange(commandPaletteOpenRequested_, false);\n''')

replace_once(
    'engine/studio_ui/include/orbit/studio_ui/StudioViewportPanels.hpp',
    '''    void Register(editor_ui::EditorUi& ui);\n    void RegisterSecondary(editor_ui::EditorUi& ui);\n\nprivate:\n''',
    '''    void Register(editor_ui::EditorUi& ui);\n    void RegisterSecondary(editor_ui::EditorUi& ui);\n\n    void RequestCommandPaletteOpen() noexcept\n    {\n        expansion_.RequestCommandPaletteOpen();\n    }\n\nprivate:\n''')

# Register the global chord only after the shell exists. `true` intentionally
# permits Ctrl+Shift+P while a text field owns keyboard focus; Undo/Redo remain
# suppressed in that situation.
replace_once(
    'apps/editor/src/Main.cpp',
    '''        orbit::studio_ui::StudioViewportPanels\n            studioViewportPanels(\n                studioViews,\n                studioSession);\n\n        orbit::volume_fields::\n''',
    '''        orbit::studio_ui::StudioViewportPanels\n            studioViewportPanels(\n                studioViews,\n                studioSession);\n\n        shortcuts.RegisterCallback(\n            {\n                .key = orbit::platform::Key::P,\n                .control = true,\n                .shift = true\n            },\n            [&studioViewportPanels]\n            {\n                studioViewportPanels.\n                    RequestCommandPaletteOpen();\n            },\n            true);\n\n        orbit::volume_fields::\n''')
