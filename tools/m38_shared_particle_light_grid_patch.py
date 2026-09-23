from pathlib import Path

def rep(path, old, new):
    p=Path(path); t=p.read_text(encoding='utf-8'); c=t.count(old)
    if c!=1: raise RuntimeError(f'{path}: expected 1 got {c}: {old!r}')
    p.write_text(t.replace(old,new,1),encoding='utf-8')

# Slot 7 is now the particle-light grid SRV, so depth moved to binding 8.
rep('engine/volume_render/src/UniversalVolumeRendererBase.inc',
'''[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth;
[[vk::binding(7, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler;''',
'''[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth;
[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler;''')

# Direct lighting gained buffer slot 3 for the same grid; sampled textures now
# begin at descriptor binding 4. Keep Texture2D/SamplerState pairs identical.
rep('engine/lighting/src/DirectLighting.cpp',
'''[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_baseRoughness;
[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_baseSampler;''',
'''[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_baseRoughness;
[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_baseSampler;''')

print('repaired M38 shared light-grid descriptor bindings')
