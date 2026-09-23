from pathlib import Path

def rep(path,old,new):
 p=Path(path); t=p.read_text(encoding='utf-8'); c=t.count(old)
 if c!=1: raise RuntimeError(f'{path}: expected 1 got {c}: {old[:100]!r}')
 p.write_text(t.replace(old,new,1),encoding='utf-8')

p='engine/volume_render/include/orbit/volume_render/VolumeParticleGpuState.hpp'
rep(p,'    bool splashCurrentIsA_{true};\n','    bool splashCurrentIsA_{true};\n    math::Double3 splashOriginDeltaMeters_{};\n')

p='engine/volume_render/src/VolumeParticleGpuState.cpp'
rep(p,
''' if(i<g.counts.z){ SplashState s=g_source[i]; if(s.generation==g.counts.x && s.lifetimeSeconds>0.0){ s.ageSeconds+=dt; if(s.ageSeconds<s.lifetimeSeconds){ s.positionMeters+=originDelta; Append(s); } } }\n uint eventCount=min(g_eventCounter[0],4096u);\n if(i<eventCount){ SplashEvent e=g_events[i]; if(e.generation==g.counts.y){''',
''' if(i<g.counts.w){ SplashState s=g_source[i]; if(s.generation==g.counts.x && s.lifetimeSeconds>0.0){ s.ageSeconds+=dt; if(s.ageSeconds<s.lifetimeSeconds){ s.positionMeters+=originDelta; Append(s); } } }\n uint eventCount=min(g_eventCounter[0],4096u);\n if(i<eventCount){ SplashEvent e=g_events[i]; if(e.generation==g.counts.z){''')
rep(p,
'''    const math::Double3 originDelta{\n        previousOriginMeters.x - newOriginMeters.x,\n        previousOriginMeters.y - newOriginMeters.y,\n        previousOriginMeters.z - newOriginMeters.z\n    };\n''',
'''    const math::Double3 originDelta{\n        previousOriginMeters.x - newOriginMeters.x,\n        previousOriginMeters.y - newOriginMeters.y,\n        previousOriginMeters.z - newOriginMeters.z\n    };\n    splashOriginDeltaMeters_ = originDelta;\n''')
rep(p,
'''    std::array<u32,8U> splashConstants{}; splashConstants[0]=previousSplashGeneration; splashConstants[1]=splashGeneration_; splashConstants[2]=MaximumPersistentSplashCount; splashConstants[3]=MaximumPersistentSplashCount; splashConstants[4]=Bits(splashDt);\n    // Splash state shares the same presentation frame; particle rebasing delta was already applied in Advance. New events are in the new frame, while old splash state needs the same delta.\n    splashConstants[5]=0U; splashConstants[6]=0U; splashConstants[7]=0U;\n''',
'''    std::array<u32,8U> splashConstants{}; splashConstants[0]=previousSplashGeneration; splashConstants[1]=splashGeneration_; splashConstants[2]=generation_; splashConstants[3]=MaximumPersistentSplashCount; splashConstants[4]=Bits(splashDt);\n    // New events are already in the new presentation frame. Only surviving splash state is rebased.\n    splashConstants[5]=Bits(static_cast<f32>(splashOriginDeltaMeters_.x)); splashConstants[6]=Bits(static_cast<f32>(splashOriginDeltaMeters_.y)); splashConstants[7]=Bits(static_cast<f32>(splashOriginDeltaMeters_.z));\n''')
rep(p,
'''    commands.Dispatch((std::max(MaximumPersistentSplashCount,MaximumSplashEventCount)+63U)/64U,1U,1U); commands.UavBarrier(splashDestination); splashCurrentIsA_=!splashCurrentIsA_;\n    TransitionState(commands,splashDestination,splashDestinationState,rhi::ResourceState::ShaderResource);\n''',
'''    commands.Dispatch((std::max(MaximumPersistentSplashCount,MaximumSplashEventCount)+63U)/64U,1U,1U); commands.UavBarrier(splashDestination); splashCurrentIsA_=!splashCurrentIsA_;\n    splashOriginDeltaMeters_ = {};\n    TransitionState(commands,splashDestination,splashDestinationState,rhi::ResourceState::ShaderResource);\n''')
rep(p,
'''    splashCurrentIsA_ = true;\n    initialized_ = false;''',
'''    splashCurrentIsA_ = true;\n    splashOriginDeltaMeters_ = {};\n    initialized_ = false;''')
print('fixed persistent splash generation + origin rebase')
