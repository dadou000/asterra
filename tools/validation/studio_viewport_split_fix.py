from pathlib import Path


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding='utf-8')
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected one anchor, found {count}')
    path.write_text(text.replace(old, new, 1), encoding='utf-8')


wrapper = Path('engine/studio_ui/src/StudioViewportPanels.cpp')
replace_once(
    wrapper,
    '''#define Register RegisterBase\n#define RegisterSecondary RegisterSecondaryBase\n#define DrawView DrawViewBase\n#include "StudioViewportPanelsBase.cpp"\n#undef DrawView\n#undef RegisterSecondary\n#undef Register\n''',
    '''#include "StudioViewportPanelsBase.cpp"\n''',
    'Remove token-renaming include macros')

base = Path('engine/studio_ui/src/StudioViewportPanelsBase.cpp')
replace_once(
    base,
    '''void StudioViewportPanels::Register(\n''',
    '''void StudioViewportPanels::RegisterBase(\n''',
    'Explicit RegisterBase definition')
replace_once(
    base,
    '''void StudioViewportPanels::RegisterSecondary(\n''',
    '''void StudioViewportPanels::RegisterSecondaryBase(\n''',
    'Explicit RegisterSecondaryBase definition')
replace_once(
    base,
    '''void StudioViewportPanels::DrawView(\n''',
    '''void StudioViewportPanels::DrawViewBase(\n''',
    'Explicit DrawViewBase definition')
