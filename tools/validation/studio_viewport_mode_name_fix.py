from pathlib import Path

path = Path('engine/studio_ui/src/StudioExpansionShell.cpp')
text = path.read_text(encoding='utf-8')
old_decl = '[[nodiscard]] const char* ViewportModeName(\n'
new_decl = '[[nodiscard]] const char* ViewportModeDisplayName(\n'
if text.count(old_decl) != 1:
    raise SystemExit(f'expected one local ViewportModeName declaration, found {text.count(old_decl)}')
text = text.replace(old_decl, new_decl, 1)
old_call = '            ViewportModeName(target->mode)));\n'
new_call = '            ViewportModeDisplayName(target->mode)));\n'
if text.count(old_call) != 1:
    raise SystemExit(f'expected one local ViewportModeName call, found {text.count(old_call)}')
text = text.replace(old_call, new_call, 1)
path.write_text(text, encoding='utf-8')
