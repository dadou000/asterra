from pathlib import Path

def rep(path, old, new, count=1):
    p=Path(path); t=p.read_text(encoding='utf-8'); c=t.count(old)
    if c < count: raise RuntimeError(f'{path}: expected >= {count}, got {c}: {old[:120]!r}')
    p.write_text(t.replace(old,new,count),encoding='utf-8')

# Header: expose shared lighting contract and per-viewport light buffer.
p='engine/volume_render/include/orbit/volume_render/VolumeParticleRenderer.hpp'
rep(p,'#include <orbit/math/Vector.hpp>\n','#include <orbit/lighting/DirectLighting.hpp>\n#include <orbit/lighting/LocalLightRegistry.hpp>\n#include <orbit/math/Vector.hpp>\n')
rep(p,
'''        math::Double3 cameraPositionRelativeToPresentationOriginMeters,\n        u32 frameIndex,\n        u64 temporalHistoryKey,\n        f32 radiusPixels = 3.0F);''',
'''        math::Double3 cameraPositionRelativeToPresentationOriginMeters,\n        u32 frameIndex,\n        u64 temporalHistoryKey,\n        const lighting::DirectionalLight& stellarLight,\n        std::span<const lighting::ResolvedLocalLight> localLights,\n        f32 radiusPixels = 3.0F);''')
rep(p,'class VolumeParticleRenderer\n{\npublic:\n','class VolumeParticleRenderer\n{\npublic:\n    static constexpr u32 MaximumLocalLightCount = 64U;\n\n')
rep(p,
'''        std::unique_ptr<rhi::Texture> historyA;\n        std::unique_ptr<rhi::Texture> historyB;''',
'''        std::unique_ptr<rhi::Texture> historyA;\n        std::unique_ptr<rhi::Texture> historyB;\n        std::unique_ptr<rhi::Buffer> localLights;''')

# Source: add lighting buffer, bindings, shading, and constants.
p='engine/volume_render/src/VolumeParticleRenderer.cpp'
t=Path(p).read_text(encoding='utf-8')

# Particle VS carries camera-relative center to PS.
t=t.replace('    float stochasticCoverage : TEXCOORD9;\n};','    float stochasticCoverage : TEXCOORD9;\n    float3 centerCameraRelative : TEXCOORD10;\n};',1)
t=t.replace('        output.softnessMeters = 0.0;\n        return output;','        output.softnessMeters = 0.0;\n        output.centerCameraRelative = 0.0;\n        return output;',2)
t=t.replace('    output.softnessMeters = max(particle.radiusMeters * 2.0, 0.05);\n    return output;','    output.softnessMeters = max(particle.radiusMeters * 2.0, 0.05);\n    output.centerCameraRelative = particle.positionMeters - g.camera.xyz;\n    return output;',1)

# Particle PS: local-light buffer + moved depth binding + expanded push contract.
old='''    float emissionScale : TEXCOORD7;    float softnessMeters : TEXCOORD8;\n    float stochasticCoverage : TEXCOORD9;\n};\n[[vk::binding(8,0)]] [[vk::combinedImageSampler]] Texture2D g_sceneDepth;\n[[vk::binding(8,0)]] [[vk::combinedImageSampler]] SamplerState g_sceneDepthSampler;\nstruct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; float4 temporal; }; [[vk::push_constant]] Push g;'''
new='''    float emissionScale : TEXCOORD7;    float softnessMeters : TEXCOORD8;\n    float stochasticCoverage : TEXCOORD9;\n    float3 centerCameraRelative : TEXCOORD10;\n};\nstruct GpuLocalLight { float4 positionType; float4 directionRange; float4 colorFlux; float4 cone; };\n[[vk::binding(8,0)]] StructuredBuffer<GpuLocalLight> g_localLights;\n[[vk::binding(9,0)]] [[vk::combinedImageSampler]] Texture2D g_sceneDepth;\n[[vk::binding(9,0)]] [[vk::combinedImageSampler]] SamplerState g_sceneDepthSampler;\nstruct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; float4 temporal; float4 stellar; float4 lighting; }; [[vk::push_constant]] Push g;'''
if old not in t: raise RuntimeError('particle pixel interface seam missing')
t=t.replace(old,new,1)

helper='''\nfloat RangeAttenuation(float d,float range){float n=d/max(range,1e-4);float q=n*n*n*n;float s=saturate(1.0-q);return s*s;}\nfloat LocalIrradiance(GpuLocalLight light,float d,float3 surfaceToLight){\n float watts=max(light.colorFlux.w,0.0)/683.0; float isSpot=step(0.5,light.positionType.w); float solidAngle=12.5663706; float angular=1.0;\n if(isSpot>0.5){float outer=clamp(light.cone.y,-1.0,1.0);solidAngle=max(6.2831853*(1.0-outer),1e-4);float spotCos=dot(-surfaceToLight,normalize(light.directionRange.xyz));angular=smoothstep(outer,max(light.cone.x,outer+1e-5),spotCos);}\n return (watts/solidAngle)*(1.0/max(d*d,0.0025))*RangeAttenuation(d,light.directionRange.w)*angular/1361.0;\n}\nfloat3 ParticleIncident(float3 p,float3 pseudoNormal,float opticalDepth){\n float3 stellarDir=normalize(g.stellar.xyz); float back=saturate(0.5-0.5*dot(pseudoNormal,stellarDir)); float stellarT=exp(-opticalDepth*lerp(0.35,1.25,back));\n float3 incident=max(g.lighting.xyz,0.0)*(0.035+max(g.stellar.w,0.0)*stellarT); uint count=min(asuint(g.lighting.w),64u);\n [loop] for(uint i=0;i<count;++i){GpuLocalLight l=g_localLights[i];float3 delta=l.positionType.xyz-p;float d=length(delta);if(d<=1e-4||d>=l.directionRange.w)continue;float3 dir=delta/d;float scale=LocalIrradiance(l,d,dir);float localBack=saturate(0.5-0.5*dot(pseudoNormal,dir));float localT=exp(-opticalDepth*lerp(0.35,1.25,localBack));incident+=max(l.colorFlux.rgb,0.0)*scale*localT;}\n return incident;\n}\n'''
needle='float Hash12(float2 p,uint seed){uint x=asuint(p.x)*1664525u+asuint(p.y)*1013904223u+seed*747796405u;x^=x>>16;x*=2246822519u;x^=x>>13;return float(x&0x00ffffffu)/16777216.0;}\n'
if needle not in t: raise RuntimeError('particle hash seam missing')
t=t.replace(needle,needle+helper,1)

old='''    const float3 densityColor = input.baseColor * lerp(0.72, 1.0, density);\n    const float3 emissiveColor = input.emissionColor * emission * input.emissionScale;\n    const float3 color = densityColor + emissiveColor;\n    const float depthFade=SoftDepth(input.position,input.softnessMeters);\n    const float alpha = soft * input.life * depthFade *\n        saturate(0.16 + 0.64 * authority + 0.20 * density);'''
new='''    const float depthFade=SoftDepth(input.position,input.softnessMeters);\n    const float alpha = soft * input.life * depthFade *\n        saturate(0.16 + 0.64 * authority + 0.20 * density);\n    const float opticalDepth = -log(max(1.0 - saturate(alpha), 1.0e-4));\n    const float3 fw=normalize(g.forward.xyz), requestedUp=normalize(g.up.xyz);\n    const float3 right=normalize(cross(fw,requestedUp)), cameraUp=normalize(cross(right,fw));\n    const float z=sqrt(saturate(1.0-radius2));\n    const float3 pseudoNormal=normalize(right*input.uv.x-cameraUp*input.uv.y-fw*z);\n    const float3 incident=ParticleIncident(input.centerCameraRelative,pseudoNormal,opticalDepth);\n    const float3 densityColor = input.baseColor * lerp(0.72, 1.0, density) * incident;\n    const float3 emissiveColor = input.emissionColor * emission * input.emissionScale;\n    const float3 color = densityColor + emissiveColor;'''
if old not in t: raise RuntimeError('particle lighting color seam missing')
t=t.replace(old,new,1)
t=t.replace('    const float opticalDepth = -log(max(1.0 - saturate(alpha), 1.0e-4));\n    output.accumulation', '    output.accumulation',1)

# All effect pipelines need slot 8 reserved so sampled scene depth moves to descriptor binding 9.
t=t.replace('[[vk::binding(8,0)]] [[vk::combinedImageSampler]] Texture2D g_sceneDepth; [[vk::binding(8,0)]] [[vk::combinedImageSampler]] SamplerState g_sceneDepthSampler;', '[[vk::binding(9,0)]] [[vk::combinedImageSampler]] Texture2D g_sceneDepth; [[vk::binding(9,0)]] [[vk::combinedImageSampler]] SamplerState g_sceneDepthSampler;',2)
t=t.replace('.pushConstantDwords = 24U,\n        .shaderResourceBuffers = 8U,', '.pushConstantDwords = 32U,\n        .shaderResourceBuffers = 9U,',1)
t=t.replace('.pushConstantDwords=24U,.shaderResourceBuffers=8U,.sampledTextures=1U,', '.pushConstantDwords=32U,.shaderResourceBuffers=9U,.sampledTextures=1U,',2)

# Expand compact splash/droplet Push layouts to match 32 dwords even though their shaders ignore lighting.
t=t.replace('struct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; float4 temporal; };', 'struct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; float4 temporal; float4 stellar; float4 lighting; };',2)

# Allocate local light buffer per viewport/frame target.
old='''        target.historyB=device_->CreateTexture({.width=width,.height=height,.format=rhi::TextureFormat::RGBA16_Float,.initialState=rhi::ResourceState::ShaderResource});\n        target.accumulationState=rhi::ResourceState::ShaderResource;'''
new='''        target.historyB=device_->CreateTexture({.width=width,.height=height,.format=rhi::TextureFormat::RGBA16_Float,.initialState=rhi::ResourceState::ShaderResource});\n        target.localLights=device_->CreateBuffer({.sizeBytes=static_cast<u64>(MaximumLocalLightCount)*sizeof(lighting::GpuLocalLight),.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::HostVisible,.initialState=rhi::ResourceState::ShaderResource});\n        target.accumulationState=rhi::ResourceState::ShaderResource;'''
if old not in t: raise RuntimeError('OIT target allocation seam missing')
t=t.replace(old,new,1)

# Draw signature.
old='''    const math::Double3 cameraPositionRelativeToPresentationOriginMeters,\n    const u32 frameIndex,\n    const u64 temporalHistoryKey,\n    const f32 radiusPixels)'''
new='''    const math::Double3 cameraPositionRelativeToPresentationOriginMeters,\n    const u32 frameIndex,\n    const u64 temporalHistoryKey,\n    const lighting::DirectionalLight& stellarLight,\n    const std::span<const lighting::ResolvedLocalLight> localLights,\n    const f32 radiusPixels)'''
if old not in t: raise RuntimeError('Draw signature seam missing')
t=t.replace(old,new,1)

# Expand constants and upload viewport-local lights.
t=t.replace('std::array<u32, 24U> constants{};', 'std::array<u32, 32U> constants{};',1)
old='''    auto& oit=EnsureOitTargets(width,height,frameIndex,temporalHistoryKey);\n    ++oit.temporalSequence; if(oit.temporalSequence==0U) oit.temporalSequence=1U;\n    constants[20]=oit.temporalSequence;'''
new='''    auto& oit=EnsureOitTargets(width,height,frameIndex,temporalHistoryKey);\n    ++oit.temporalSequence; if(oit.temporalSequence==0U) oit.temporalSequence=1U;\n    constants[20]=oit.temporalSequence;\n    constants[24]=bits(stellarLight.directionToLight.x); constants[25]=bits(stellarLight.directionToLight.y); constants[26]=bits(stellarLight.directionToLight.z); constants[27]=bits(std::max(stellarLight.irradianceScale,0.0F));\n    constants[28]=bits(std::max(stellarLight.colorLinear.x,0.0F)); constants[29]=bits(std::max(stellarLight.colorLinear.y,0.0F)); constants[30]=bits(std::max(stellarLight.colorLinear.z,0.0F));\n    const u32 localLightCount=std::min<u32>(static_cast<u32>(localLights.size()),MaximumLocalLightCount); constants[31]=localLightCount;\n    if(oit.localLights==nullptr) throw std::logic_error("M38 viewport-local light buffer is unavailable.");\n    auto* mappedLights=oit.localLights->Map(); std::memset(mappedLights,0,static_cast<std::size_t>(oit.localLights->SizeBytes()));\n    auto* encodedLights=reinterpret_cast<lighting::GpuLocalLight*>(mappedLights); for(u32 i=0;i<localLightCount;++i) encodedLights[i]=lighting::EncodeGpuLocalLight(localLights[i]); oit.localLights->Unmap();'''
if old not in t: raise RuntimeError('Draw constants seam missing')
t=t.replace(old,new,1)

# Bind viewport-local lights for all three effect pipelines.
t=t.replace('    state_.BindForGraphics(commands);\n    commands.SetGraphicsTexture(0U,depth);', '    state_.BindForGraphics(commands);\n    commands.SetGraphicsBuffer(8U,*oit.localLights);\n    commands.SetGraphicsTexture(0U,depth);',3)

Path(p).write_text(t,encoding='utf-8')

# Studio: resolve same authored lights for particles and pass stellar/local authority.
p='engine/studio_ui/src/StudioViewportRenderer.cpp'
t=Path(p).read_text(encoding='utf-8')
old='''            if (particleOutputGeneration_ > 0U)\n            {\n                const auto camera =\n                    view->Camera();'''
new='''            if (particleOutputGeneration_ > 0U)\n            {\n                const auto camera =\n                    view->Camera();\n                std::optional<scene::ObjectId> particleLightRoot;\n                if(logicalTarget->target.has_value() && snapshot.hasWorld)\n                {\n                    particleLightRoot=session.World().Universe().ObjectForBody(logicalTarget->target->body);\n                }\n                const auto particleLocalLights=ResolveStudioLocalLights(session,view->Lighting(),particleLightRoot);\n                const lighting::DirectionalLight particleStellarLight{.directionToLight=studioDirectLight.directionBody,.colorLinear={1.0F,1.0F,1.0F},.irradianceScale=studioDirectLight.irradianceScale};'''
if old not in t: raise RuntimeError('Studio particle lighting setup seam missing')
t=t.replace(old,new,1)

old='''                     particleFrameIndex,\n                     particleTemporalHistoryKey,\n                     advanceParticleState,'''
new='''                     particleFrameIndex,\n                     particleTemporalHistoryKey,\n                     particleStellarLight,\n                     particleLocalLights,\n                     advanceParticleState,'''
if old not in t: raise RuntimeError('Studio capture seam missing')
t=t.replace(old,new,1)

old='''                            cameraRelativeToParticleOrigin,\n                            particleFrameIndex,\n                            particleTemporalHistoryKey);'''
new='''                            cameraRelativeToParticleOrigin,\n                            particleFrameIndex,\n                            particleTemporalHistoryKey,\n                            particleStellarLight,\n                            particleLocalLights);'''
if old not in t: raise RuntimeError('Studio Draw call seam missing')
t=t.replace(old,new,1)
Path(p).write_text(t,encoding='utf-8')

# Regression contract.
p='engine/studio_ui/tests/VolumeParticleRenderBridgeTests.cpp'
t=Path(p).read_text(encoding='utf-8')
needle='''    Check(rhi::TextureFormatBytesPerTexel(rhi::TextureFormat::R16_Float) == 2U);'''
if needle not in t: raise RuntimeError('test seam missing')
t=t.replace(needle,needle+'\n    Check(volume_render::VolumeParticleRenderer::MaximumLocalLightCount == 64U);',1)
Path(p).write_text(t,encoding='utf-8')

print('M38 particle lighting patch applied')
