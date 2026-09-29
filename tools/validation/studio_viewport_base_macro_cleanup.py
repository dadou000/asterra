from pathlib import Path

# The public wrapper owns panel registration. The included base implementation
# only needs the heavy DrawView body; keeping Register aliases caused the
# preprocessor to rewrite unrelated CommandRegistry::Register calls.
header_path = Path('engine/studio_ui/include/orbit/studio_ui/StudioViewportPanels.hpp')
header = header_path.read_text(encoding='utf-8')
old = '''    void RegisterBase(editor_ui::EditorUi& ui);\n    void RegisterSecondaryBase(editor_ui::EditorUi& ui);\n\n'''
if header.count(old) != 1:
    raise SystemExit(f'expected one base registration declaration block, found {header.count(old)}')
header_path.write_text(header.replace(old, '', 1), encoding='utf-8')

source_path = Path('engine/studio_ui/src/StudioViewportPanels.cpp')
source = source_path.read_text(encoding='utf-8')
old = '''#define Register RegisterBase\n#define RegisterSecondary RegisterSecondaryBase\n#define DrawView DrawViewBase\n#include "StudioViewportPanelsBase.cpp"\n#undef DrawView\n#undef RegisterSecondary\n#undef Register\n'''
new = '''#define DrawView DrawViewBase\n#include "StudioViewportPanelsBase.cpp"\n#undef DrawView\n'''
if source.count(old) != 1:
    raise SystemExit(f'expected one base include macro block, found {source.count(old)}')
source_path.write_text(source.replace(old, new, 1), encoding='utf-8')

base_path = Path('engine/studio_ui/src/StudioViewportPanelsBase.cpp')
base = base_path.read_text(encoding='utf-8')
start_marker = 'void StudioViewportPanels::Register(\n'
end_marker = 'void StudioViewportPanels::DrawView(\n'
start = base.find(start_marker)
if start < 0:
    raise SystemExit('base Register definition not found')
end = base.find(end_marker, start)
if end < 0:
    raise SystemExit('base DrawView definition not found')
removed = base[start:end]
if 'void StudioViewportPanels::RegisterSecondary(' not in removed:
    raise SystemExit('expected redundant RegisterSecondary inside removal block')
base_path.write_text(base[:start] + base[end:], encoding='utf-8')
