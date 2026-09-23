from pathlib import Path

def rep(path,old,new):
 p=Path(path); t=p.read_text(); c=t.count(old)
 if c!=1: raise RuntimeError(f'{path}: expected 1 got {c}: {old[:120]!r}')
 p.write_text(t.replace(old,new,1))

def repn(path,old,new,count):
 p=Path(path); t=p.read_text(); c=t.count(old)
 if c!=count: raise RuntimeError(f'{path}: expected {count} got {c}: {old[:120]!r}')
 p.write_text(t.replace(old,new,count))

p='engine/volume_render/src/VolumeParticleRenderer.cpp'
rep(p,'[numthreads(64,1,1)] void main(uint3 tid:SV_DispatchThreadID){ uint i=tid.x; if(i>=65536u) return;', '[numthreads(64,1,1)] void main(uint3 tid:SV_DispatchThreadID){ uint i=tid.x; if(i==0u) g_grid[0]=uint4(asuint(g.originCell.x),asuint(g.originCell.y),asuint(g.originCell.z),asuint(g.originCell.w)); if(i>=65536u) return;')
rep(p,'uint idx=(uint(c.z)*r+uint(c.y))*r+uint(c.x);', 'uint idx=1u+(uint(c.z)*r+uint(c.y))*r+uint(c.x);')
rep(p,'[[vk::binding(8,0)]] StructuredBuffer<GpuLocalLight> g_localLights;\n[[vk::binding(9,0)]] [[vk::combinedImageSampler]] Texture2D g_sceneDepth;\n[[vk::binding(9,0)]] [[vk::combinedImageSampler]] SamplerState g_sceneDepthSampler;', '''[[vk::binding(8,0)]] StructuredBuffer<GpuLocalLight> g_localLights;
[[vk::binding(9,0)]] StructuredBuffer<uint4> g_particleLightGrid;
[[vk::binding(10,0)]] [[vk::combinedImageSampler]] Texture2D g_sceneDepth;
[[vk::binding(10,0)]] [[vk::combinedImageSampler]] SamplerState g_sceneDepthSampler;''')
insert='''\nfloat GridOptical(float3 p){ uint4 meta=g_particleLightGrid[0]; float3 o=float3(asfloat(meta.x),asfloat(meta.y),asfloat(meta.z)); float cell=max(asfloat(meta.w),1e-4); int3 c=int3(floor((p-o)/cell)); const int r=32; if(any(c<0)||any(c>=int3(r,r,r))) return 0.0; uint idx=1u+(uint(c.z)*r+uint(c.y))*r+uint(c.x); return min(float(g_particleLightGrid[idx].x)/4096.0,20.0); }\nfloat GridTransmittance(float3 p,float3 dir,float maxDistance){ float3 d=normalize(dir); float optical=0.0; float stepLen=max(maxDistance/6.0,8.0); [unroll] for(uint s=1u;s<=6u;++s){ float dist=min(stepLen*float(s),maxDistance); optical+=GridOptical(p+d*dist)*0.18; } return exp(-min(optical,20.0)); }\n'''
rep(p,'struct OitOutput { float4 accumulation : SV_Target0; float4 opticalDepth : SV_Target1; float4 motionReject : SV_Target2; };',insert+'struct OitOutput { float4 accumulation : SV_Target0; float4 opticalDepth : SV_Target1; float4 motionReject : SV_Target2; };')
rep(p,'    const float3 incident=ParticleIncident(input.centerCameraRelative,pseudoNormal,opticalDepth);', '''    float3 incident=ParticleIncident(input.centerCameraRelative,pseudoNormal,opticalDepth);
    const float stellarGridT=GridTransmittance(input.centerCameraRelative,normalize(g.stellar.xyz),192.0);
    incident*=lerp(1.0,stellarGridT,saturate(g.stellar.w));''')
rep(p,'.pushConstantDwords = 32U,\n        .shaderResourceBuffers = 9U,', '.pushConstantDwords = 32U,\n        .shaderResourceBuffers = 10U,')
repn(p,'.vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=32U,.shaderResourceBuffers=9U,.sampledTextures=1U,', '.vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=32U,.shaderResourceBuffers=10U,.sampledTextures=1U,',2)
rep(p,'constexpr u64 gridBytes=static_cast<u64>(ParticleLightGridResolution)*ParticleLightGridResolution*ParticleLightGridResolution*sizeof(std::array<u32,4U>);', 'constexpr u64 gridBytes=(1ULL+static_cast<u64>(ParticleLightGridResolution)*ParticleLightGridResolution*ParticleLightGridResolution)*sizeof(std::array<u32,4U>);')
text=Path(p).read_text()
needle='    state_.BindForGraphics(commands);\n    commands.SetGraphicsTexture(0U,depth);'
if text.count(needle)!=3: raise RuntimeError(f'expected 3 draw bindings, got {text.count(needle)}')
text=text.replace(needle,'    state_.BindForGraphics(commands);\n    commands.SetGraphicsBuffer(8U,*oit.localLights);\n    commands.SetGraphicsBuffer(9U,*particleLightGrid_);\n    commands.SetGraphicsTexture(0U,depth);')
Path(p).write_text(text)

p='engine/studio_ui/tests/VolumeParticleRenderBridgeTests.cpp'
rep(p,'    Check(volume_render::VolumeParticleRenderer::ParticleLightGridResolution == 32U);\n','    Check(volume_render::VolumeParticleRenderer::ParticleLightGridResolution == 32U);\n    Check((1U + volume_render::VolumeParticleRenderer::ParticleLightGridResolution * volume_render::VolumeParticleRenderer::ParticleLightGridResolution * volume_render::VolumeParticleRenderer::ParticleLightGridResolution) == 32769U);\n')
print('grid shadow patch applied')
