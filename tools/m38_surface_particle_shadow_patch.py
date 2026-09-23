from pathlib import Path

def load(p): return Path(p).read_text(encoding='utf-8')
def save(p,t): Path(p).write_text(t,encoding='utf-8')
def rep(p,old,new,count=1):
 t=load(p); c=t.count(old)
 if c!=count: raise RuntimeError(f'{p}: expected {count} got {c}: {old[:160]!r}')
 save(p,t.replace(old,new,count))

# Direct lighting API + dummy grid.
p='engine/lighting/include/orbit/lighting/DirectLighting.hpp'
rep(p,
'''        const DirectionalLight& light,\n        const TiledLightGrid& localLightGrid,\n        const DirectLightingSettings& settings = {});''',
'''        const DirectionalLight& light,\n        const TiledLightGrid& localLightGrid,\n        rhi::Buffer* particleLightGrid = nullptr,\n        const DirectLightingSettings& settings = {});''')
rep(p,
'''private:\n    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;''',
'''private:\n    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;\n    std::unique_ptr<rhi::Buffer> dummyParticleLightGrid_;''')

p='engine/lighting/src/DirectLighting.cpp'
rep(p,'#include <cmath>\n','#include <cmath>\n#include <cstring>\n')
rep(p,
'''[[vk::binding(2, 0)]]\nStructuredBuffer<uint> g_tileLightIndices;\n\n[[vk::binding(3, 0)]]''',
'''[[vk::binding(2, 0)]]\nStructuredBuffer<uint> g_tileLightIndices;\n[[vk::binding(3, 0)]]\nStructuredBuffer<uint4> g_particleLightGrid;\n\n[[vk::binding(4, 0)]]''')
rep(p,'[[vk::binding(4, 0)]]\n[[vk::combinedImageSampler]]\nTexture2D g_normalMetallic;', '[[vk::binding(5, 0)]]\n[[vk::combinedImageSampler]]\nTexture2D g_normalMetallic;')
rep(p,'[[vk::binding(4, 0)]]\n[[vk::combinedImageSampler]]\nSamplerState g_normalSampler;', '[[vk::binding(5, 0)]]\n[[vk::combinedImageSampler]]\nSamplerState g_normalSampler;')
rep(p,'[[vk::binding(5, 0)]]\n[[vk::combinedImageSampler]]\nTexture2D g_emissionClass;', '[[vk::binding(6, 0)]]\n[[vk::combinedImageSampler]]\nTexture2D g_emissionClass;')
rep(p,'[[vk::binding(5, 0)]]\n[[vk::combinedImageSampler]]\nSamplerState g_emissionSampler;', '[[vk::binding(6, 0)]]\n[[vk::combinedImageSampler]]\nSamplerState g_emissionSampler;')
rep(p,'[[vk::binding(6, 0)]]\n[[vk::combinedImageSampler]]\nTexture2D g_depth;', '[[vk::binding(7, 0)]]\n[[vk::combinedImageSampler]]\nTexture2D g_depth;')
rep(p,'[[vk::binding(6, 0)]]\n[[vk::combinedImageSampler]]\nSamplerState g_depthSampler;', '[[vk::binding(7, 0)]]\n[[vk::combinedImageSampler]]\nSamplerState g_depthSampler;')
rep(p,
'''    uint4 localGrid;\n};''',
'''    uint4 localGrid;\n    float4 cameraFrameAndParticleGrid;\n};''')
helper='''\nfloat4 SampleParticleLightGrid(float3 framePosition)\n{\n    if (g.cameraFrameAndParticleGrid.w <= 0.0) return 0.0;\n    const uint4 meta = g_particleLightGrid[1];\n    const float3 origin = float3(asfloat(meta.x),asfloat(meta.y),asfloat(meta.z));\n    const float cellSize = asfloat(meta.w);\n    if (!(cellSize > 0.0)) return 0.0;\n    const int3 cell = int3(floor((framePosition-origin)/cellSize));\n    if (any(cell < 0) || any(cell >= int3(32,32,32))) return 0.0;\n    const uint index = 2u + (uint(cell.z)*32u + uint(cell.y))*32u + uint(cell.x);\n    const uint4 packed = g_particleLightGrid[index];\n    return float4(float(packed.x)/4096.0, float3(packed.y,packed.z,packed.w)/1024.0);\n}\n\nfloat ParticleGridTransmittance(float3 framePosition,float3 direction,float maximumDistance)\n{\n    if (g.cameraFrameAndParticleGrid.w <= 0.0 || maximumDistance <= 1.0e-4) return 1.0;\n    const float3 d=normalize(direction);\n    const float distance=min(maximumDistance,192.0);\n    const float stepLength=max(distance/6.0,8.0);\n    float optical=0.0;\n    [unroll] for(uint i=1u;i<=6u;++i)\n    {\n        const float t=min(stepLength*float(i),distance);\n        optical += SampleParticleLightGrid(framePosition+d*t).x * 0.18;\n    }\n    return exp(-min(optical,20.0));\n}\n'''
rep(p,
'''float4 main(VSOutput input) : SV_Target0\n{''',
helper+'''\nfloat4 main(VSOutput input) : SV_Target0\n{''')
rep(p,
'''    float3 sceneLinear =\n        EvaluateBrdf(\n            n,\n            v,\n            stellarDirection,\n            baseColor,\n            roughness,\n            metallic) *\n        stellarColor *\n        stellarIrradiance;\n\n    const float ambient =\n        max(g.lightColorAndAmbient.w, 0.0);\n\n    sceneLinear +=\n        baseColor *\n        (1.0 - metallic) *\n        ambient;''',
'''    float3 stellarLinear =\n        EvaluateBrdf(\n            n,\n            v,\n            stellarDirection,\n            baseColor,\n            roughness,\n            metallic) *\n        stellarColor *\n        stellarIrradiance;\n\n    const float ambient =\n        max(g.lightColorAndAmbient.w, 0.0);\n    const float3 ambientLinear =\n        baseColor *\n        (1.0 - metallic) *\n        ambient;\n    float3 sceneLinear = stellarLinear + ambientLinear;''')
# surface grid effect immediately after depth sample
rep(p,
'''    const float depth =\n        g_depth.Sample(\n            g_depthSampler,\n            input.uv).r;\n\n    // Reverse-Z depth 0 is''',
'''    const float depth =\n        g_depth.Sample(\n            g_depthSampler,\n            input.uv).r;\n\n    if (depth > 0.0 && g.cameraFrameAndParticleGrid.w > 0.0)\n    {\n        const float3 surfaceRelative =\n            ReconstructSurfacePosition(depth,cameraRay,cameraForward);\n        const float3 framePosition =\n            surfaceRelative + g.cameraFrameAndParticleGrid.xyz;\n        const float stellarParticleT =\n            ParticleGridTransmittance(framePosition,stellarDirection,192.0);\n        const float3 particleEmission =\n            SampleParticleLightGrid(framePosition).yzw;\n        sceneLinear = stellarLinear * stellarParticleT + ambientLinear + particleEmission;\n    }\n\n    // Reverse-Z depth 0 is''')
# local lights multiply particle grid transmittance
rep(p,
'''            sceneLinear +=\n                EvaluateBrdf(\n                    n,\n                    v,\n                    l,\n                    baseColor,\n                    roughness,\n                    metallic) *\n                max(local.colorFlux.rgb, 0.0) *\n                irradianceScale;''',
'''            const float3 framePosition =\n                surfacePosition + g.cameraFrameAndParticleGrid.xyz;\n            const float particleT =\n                ParticleGridTransmittance(framePosition,l,distanceMeters);\n            sceneLinear +=\n                EvaluateBrdf(\n                    n,\n                    v,\n                    l,\n                    baseColor,\n                    roughness,\n                    metallic) *\n                max(local.colorFlux.rgb, 0.0) *\n                irradianceScale * particleT;''')
rep(p,'.pushConstantDwords = 24U,\n            .shaderResourceBuffers = 3U,','.pushConstantDwords = 28U,\n            .shaderResourceBuffers = 4U,')
# Create dummy after pipeline construction
rep(p,
'''            .colorAttachmentCount = 1U\n        });\n}''',
'''            .colorAttachmentCount = 1U\n        });\n\n    dummyParticleLightGrid_ = device.CreateBuffer({\n        .sizeBytes = 2U * sizeof(std::array<u32,4U>),\n        .usage = rhi::BufferUsage::Structured,\n        .memory = rhi::MemoryUsage::HostVisible,\n        .initialState = rhi::ResourceState::ShaderResource\n    });\n    std::memset(dummyParticleLightGrid_->Map(),0,static_cast<std::size_t>(dummyParticleLightGrid_->SizeBytes()));\n    dummyParticleLightGrid_->Unmap();\n}''')
rep(p,
'''    const DirectionalLight& light,\n    const TiledLightGrid& localLightGrid,\n    const DirectLightingSettings& settings)''',
'''    const DirectionalLight& light,\n    const TiledLightGrid& localLightGrid,\n    rhi::Buffer* particleLightGrid,\n    const DirectLightingSettings& settings)''')
# constants 24 -> 28, append camera absolute+enabled
rep(p,'const std::array<u32, 24> constants{','const std::array<u32, 28> constants{')
rep(p,
'''        static_cast<u32>(\n            localLightGrid.lights.size())\n    };''',
'''        static_cast<u32>(\n            localLightGrid.lights.size()),\n\n        bits(static_cast<f32>(view.cameraPositionInFrameMeters.x)),\n        bits(static_cast<f32>(view.cameraPositionInFrameMeters.y)),\n        bits(static_cast<f32>(view.cameraPositionInFrameMeters.z)),\n        bits(particleLightGrid != nullptr ? 1.0F : 0.0F)\n    };''')
rep(p,
'''    commands.SetGraphicsBuffer(\n        2U,\n        tileLightIndices);\n\n    commands.SetGraphicsTexture(''',
'''    commands.SetGraphicsBuffer(\n        2U,\n        tileLightIndices);\n    commands.SetGraphicsBuffer(\n        3U,\n        particleLightGrid != nullptr\n            ? *particleLightGrid\n            : *dummyParticleLightGrid_);\n\n    commands.SetGraphicsTexture(''')

# Studio: capture previous ready grid pointer into DirectLighting pass.
p='engine/studio_ui/src/StudioViewportRenderer.cpp'
# define pointer before direct graph pass using exact runtime override block end seam
marker='''            graph.AddPass(\n                prefix + ".SharedDirectLighting",'''
insert='''            rhi::Buffer* particleLightGridForDirect =\n                volumeParticleRenderer_.ParticleLightGridReady()\n                    ? &volumeParticleRenderer_.ParticleLightGrid()\n                    : nullptr;\n\n'''
t=load(p)
if t.count(marker)!=1: raise RuntimeError(f'{p}: direct pass marker {t.count(marker)}')
t=t.replace(marker,insert+marker,1); save(p,t)
# capture pointer
rep(p,
'''                 localIndicesHandle,\n                 lightingTimestamps,''',
'''                 localIndicesHandle,\n                 particleLightGridForDirect,\n                 lightingTimestamps,''')
rep(p,
'''                        directLight,\n                        localLightGrid);''',
'''                        directLight,\n                        localLightGrid,\n                        particleLightGridForDirect);''')

print('M38 surface particle shadow patch applied')
