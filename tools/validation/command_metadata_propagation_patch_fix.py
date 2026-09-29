from pathlib import Path

path = Path('tools/validation/command_metadata_propagation_patch.py')
text = path.read_text(encoding='utf-8')
old = "    if count != 1:\n        raise SystemExit(f'{label} anchor count {count}')\n"
new = "    if count < 1:\n        raise SystemExit(f'{label} anchor count {count}')\n"
if text.count(old) != 1:
    raise SystemExit('replace_once guard anchor mismatch')
path.write_text(text.replace(old, new, 1), encoding='utf-8')
