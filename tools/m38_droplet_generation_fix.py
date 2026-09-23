from pathlib import Path
p=Path('engine/volume_render/src/VolumeParticleGpuState.cpp')
t=p.read_text(encoding='utf-8')
old='''d.bodyIdentity=e.bodyIdentity; d.generation=g.counts.y; d.flags=0u; d.reserved=0u; Append(d);'''
new='''d.bodyIdentity=e.bodyIdentity; d.generation=g.counts.y; d.flags=e.generation; d.reserved=0u; Append(d);'''
if t.count(old)!=1: raise RuntimeError('droplet spawn generation seam mismatch')
t=t.replace(old,new,1)
old='''e.tint=d.tint; e.generation=g.meta.y; e.bodyCenterMeters=d.bodyCenterMeters;'''
new='''e.tint=d.tint; e.generation=d.flags; e.bodyCenterMeters=d.bodyCenterMeters;'''
if t.count(old)!=1: raise RuntimeError('child splash generation seam mismatch')
t=t.replace(old,new,1)
p.write_text(t,encoding='utf-8')
print('fixed child splash source generation')
