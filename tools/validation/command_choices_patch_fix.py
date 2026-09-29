from pathlib import Path

path = Path('tools/validation/command_choices_patch.py')
text = path.read_text(encoding='utf-8')
replacements = [
    (
        '''    ''' + "'''                .displayName = \"Source Type\",\\n                .description =\\n                    \"Source adapter used by the selected universal Volume.\"\\n'''" + ''',\n''',
        '''    ''' + "'''                .displayName = \"Source Type\",\\n                .description =\\n                    \"Source adapter: Brush, Texture / Mask, Terrain, Spline, Mesh / SDF, Collision Proxy, Particles, Object Motion, or World Motion.\"\\n'''" + ''',\n'''
    ),
    (
        '''    ''' + "'''                .displayName = \"Effector Type\",\\n                .description =\\n                    \"Obstacle or force behavior applied by the selected universal Volume.\"\\n'''" + ''',\n''',
        '''    ''' + "'''                .displayName = \"Effector Type\",\\n                .description =\\n                    \"Effector type: Obstacle, Drag, Wind, Temperature, or Dissipation.\"\\n'''" + ''',\n'''
    )
]
for old, new in replacements:
    if text.count(old) != 1:
        raise SystemExit(f'anchor rewrite count {text.count(old)}')
    text = text.replace(old, new, 1)
path.write_text(text, encoding='utf-8')
