from pathlib import Path
import runpy

p = Path('tools/m38_water_behavior_patch.py')
text = p.read_text(encoding='utf-8')
old = """'''        const u32 behaviorFlags =\\n            (static_cast<u32>(request.gravityMode) & 0x3U) |\\n            ((static_cast<u32>(request.collisionMode) & 0x3U) << 2U);\\n'''"""
new = """'''        const u32 behaviorFlags =\\n            (static_cast<u32>(request.gravityMode) & 0x3U) |\\n            ((static_cast<u32>(request.collisionMode) & 0x3U) << 2U) |\\n            (event.physics.hasPhysicalSurface ? (1U << 4U) : 0U);\\n'''"""
if old not in text:
    raise RuntimeError('Could not retarget current behaviorFlags source seam')
text = text.replace(old, new, 1)
text = text.replace('event.owningBody', 'event.physics.body')
p.write_text(text, encoding='utf-8')
runpy.run_path(str(p), run_name='__main__')
