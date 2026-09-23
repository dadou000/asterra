from pathlib import Path

def load(p): return Path(p).read_text(encoding='utf-8')
def save(p,t): Path(p).write_text(t,encoding='utf-8')
def rep(p,old,new,count=1):
 t=load(p); c=t.count(old)
 if c!=count: raise RuntimeError(f'{p}: expected {count} got {c}: {old[:160]!r}')
 save(p,t.replace(old,new,count))

def ins_after(p,marker,text,count=1):
 t=load(p); c=t.count(marker)
 if c!=count: raise RuntimeError(f'{p}: marker expected {count} got {c}: {marker[:160]!r}')
 save(p,t.replace(marker,marker+text,count))

# ------------------------------------------------------------------
# Particle renderer: dual metadata + readiness + coordinate fix.
# ------------------------------------------------------------------
p='engine/volume_render/include/orbit/volume_render/VolumeParticleRenderer.hpp'
rep(p,
'''    [[nodiscard]] rhi::Buffer& ParticleLightGrid() noexcept;\n''',
'''    [[nodiscard]] rhi::Buffer& ParticleLightGrid() noexcept;\n    [[nodiscard]] bool ParticleLightGridReady() const noexcept;\n''')
rep(p,
'''    rhi::ResourceState particleLightGridState_{rhi::ResourceState::CopyDestination};\n''',
'''    rhi::ResourceState particleLightGridState_{rhi::ResourceState::CopyDestination};\n    bool particleLightGridReady_{false};\n''')

p='engine/volume_render/src/VolumeParticleRenderer.cpp'
rep(p,
'''    float3 centerCameraRelative : TEXCOORD10;\n};''',
'''    float3 centerCameraRelative : TEXCOORD10;\n    float3 gridPosition : TEXCOORD11;\n};''',2)
# two early-return blocks in particle VS
rep(p,
'''        output.stochasticCoverage = 0.0;\n        return output;''',
'''        output.stochasticCoverage = 0.0;\n        output.centerCameraRelative = 0.0;\n        output.gridPosition = 0.0;\n        return output;''',2)
rep(p,
'''    output.stochasticCoverage = saturate((projectedRadiusPixels * projectedRadiusPixels) / max(radiusPixels * radiusPixels, 1.0e-4));\n    return output;''',
'''    output.stochasticCoverage = saturate((projectedRadiusPixels * projectedRadiusPixels) / max(radiusPixels * radiusPixels, 1.0e-4));\n    output.centerCameraRelative = particle.positionMeters - g.camera.xyz;\n    output.gridPosition = particle.positionMeters;\n    return output;''')
rep(p,
'''float GridOptical(float3 p){ uint4 meta=g_particleLightGrid[0]; float3 o=float3(asfloat(meta.x),asfloat(meta.y),asfloat(meta.z)); float cell=max(asfloat(meta.w),1e-4); int3 c=int3(floor((p-o)/cell)); const int r=32; if(any(c<0)||any(c>=int3(r,r,r))) return 0.0; uint idx=1u+(uint(c.z)*r+uint(c.y))*r+uint(c.x); return min(float(g_particleLightGrid[idx].x)/4096.0,20.0); }''',
'''float GridOptical(float3 p){ uint4 meta=g_particleLightGrid[0]; float3 o=float3(asfloat(meta.x),asfloat(meta.y),asfloat(meta.z)); float cell=max(asfloat(meta.w),1e-4); int3 c=int3(floor((p-o)/cell)); const int r=32; if(any(c<0)||any(c>=int3(r,r,r))) return 0.0; uint idx=2u+(uint(c.z)*r+uint(c.y))*r+uint(c.x); return min(float(g_particleLightGrid[idx].x)/4096.0,20.0); }''')
rep(p,
'''    const float stellarGridT=GridTransmittance(input.centerCameraRelative,normalize(g.stellar.xyz),192.0);''',
'''    const float stellarGridT=GridTransmittance(input.gridPosition,normalize(g.stellar.xyz),192.0);''')
rep(p,
'''struct Push { float4 originCell; uint4 params; }; [[vk::push_constant]] Push g;\n[numthreads(64,1,1)] void main(uint3 tid:SV_DispatchThreadID){ uint i=tid.x; if(i==0u) g_grid[0]=uint4(asuint(g.originCell.x),asuint(g.originCell.y),asuint(g.originCell.z),asuint(g.originCell.w)); if(i>=65536u) return; Particle p=g_particles[i]; if(p.generation!=g.params.x||p.lifetimeSeconds<=0.0||p.ageSeconds>=p.lifetimeSeconds) return; float3 q=(p.positionMeters-g.originCell.xyz)/max(g.originCell.w,1e-4); int3 c=int3(floor(q)); uint r=g.params.y; if(any(c<0)||any(c>=int3(r,r,r))) return; uint idx=1u+(uint(c.z)*r+uint(c.y))*r+uint(c.x);''',
'''struct Push { float4 originCell; float4 frameOrigin; uint4 params; }; [[vk::push_constant]] Push g;\n[numthreads(64,1,1)] void main(uint3 tid:SV_DispatchThreadID){ uint i=tid.x; if(i==0u){ g_grid[0]=uint4(asuint(g.originCell.x),asuint(g.originCell.y),asuint(g.originCell.z),asuint(g.originCell.w)); float3 absoluteOrigin=g.originCell.xyz+g.frameOrigin.xyz; g_grid[1]=uint4(asuint(absoluteOrigin.x),asuint(absoluteOrigin.y),asuint(absoluteOrigin.z),asuint(g.originCell.w)); } if(i>=65536u) return; Particle p=g_particles[i]; if(p.generation!=g.params.x||p.lifetimeSeconds<=0.0||p.ageSeconds>=p.lifetimeSeconds) return; float3 q=(p.positionMeters-g.originCell.xyz)/max(g.originCell.w,1e-4); int3 c=int3(floor(q)); uint r=g.params.y; if(any(c<0)||any(c>=int3(r,r,r))) return; uint idx=2u+(uint(c.z)*r+uint(c.y))*r+uint(c.x);''')
rep(p,
'''particleLightGridPipeline_=device.CreateComputePipeline({.computeShader={.data=particleLightGrid.bytecode.data(),.size=particleLightGrid.bytecode.size()},.pushConstantDwords=8U,.shaderResourceBuffers=2U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});\n    constexpr u64 gridBytes=(1ULL+static_cast<u64>(ParticleLightGridResolution)*ParticleLightGridResolution*ParticleLightGridResolution)*sizeof(std::array<u32,4U>);''',
'''particleLightGridPipeline_=device.CreateComputePipeline({.computeShader={.data=particleLightGrid.bytecode.data(),.size=particleLightGrid.bytecode.size()},.pushConstantDwords=12U,.shaderResourceBuffers=2U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});\n    constexpr u64 gridBytes=(2ULL+static_cast<u64>(ParticleLightGridResolution)*ParticleLightGridResolution*ParticleLightGridResolution)*sizeof(std::array<u32,4U>);''')
rep(p,
'''    std::array<u32,8U> lightGridConstants{bits(lightGridOrigin.x),bits(lightGridOrigin.y),bits(lightGridOrigin.z),bits(lightGridCellMeters),state_.Generation(),ParticleLightGridResolution,0U,0U};\n''',
'''    const math::Double3 presentationOrigin{\n        camera.localPositionMeters.x-cameraPositionRelativeToPresentationOriginMeters.x,\n        camera.localPositionMeters.y-cameraPositionRelativeToPresentationOriginMeters.y,\n        camera.localPositionMeters.z-cameraPositionRelativeToPresentationOriginMeters.z};\n    std::array<u32,12U> lightGridConstants{\n        bits(lightGridOrigin.x),bits(lightGridOrigin.y),bits(lightGridOrigin.z),bits(lightGridCellMeters),\n        bits(static_cast<f32>(presentationOrigin.x)),bits(static_cast<f32>(presentationOrigin.y)),bits(static_cast<f32>(presentationOrigin.z)),0U,\n        state_.Generation(),ParticleLightGridResolution,0U,0U};\n''')
rep(p,
'''    commands.Transition(*particleLightGrid_,particleLightGridState_,rhi::ResourceState::ShaderResource); particleLightGridState_=rhi::ResourceState::ShaderResource;\n''',
'''    commands.Transition(*particleLightGrid_,particleLightGridState_,rhi::ResourceState::ShaderResource); particleLightGridState_=rhi::ResourceState::ShaderResource; particleLightGridReady_=true;\n''')
rep(p,
'''    particleLightGridState_=rhi::ResourceState::CopyDestination;\n}''',
'''    particleLightGridState_=rhi::ResourceState::CopyDestination;\n    particleLightGridReady_=false;\n}''')
rep(p,
'''rhi::Buffer& VolumeParticleRenderer::ParticleLightGrid() noexcept\n{\n    return *particleLightGrid_;\n}\n''',
'''rhi::Buffer& VolumeParticleRenderer::ParticleLightGrid() noexcept\n{\n    return *particleLightGrid_;\n}\n\nbool VolumeParticleRenderer::ParticleLightGridReady() const noexcept\n{\n    return particleLightGridReady_;\n}\n''')

# ------------------------------------------------------------------
# Universal volume public API: optional particle light-grid handle.
# ------------------------------------------------------------------
p='engine/volume_render/include/orbit/volume_render/UniversalVolumeRenderer.hpp'
rep(p,
'''        render_graph::BufferHandle radianceLevels,\n        u32 radianceLevelCount,\n        bool resetHistory);''',
'''        render_graph::BufferHandle radianceLevels,\n        u32 radianceLevelCount,\n        render_graph::BufferHandle particleLightGrid,\n        bool resetHistory);''',3)

p='engine/volume_render/src/VolumeRepresentationPass.cpp'
rep(p,
'''    const render_graph::BufferHandle radianceLevels,\n    const u32 radianceLevelCount,\n    const bool resetHistory)''',
'''    const render_graph::BufferHandle radianceLevels,\n    const u32 radianceLevelCount,\n    const render_graph::BufferHandle particleLightGrid,\n    const bool resetHistory)''')
rep(p,
'''        radianceLevels,\n        radianceLevelCount,\n        resetHistory);''',
'''        radianceLevels,\n        radianceLevelCount,\n        particleLightGrid,\n        resetHistory);''')

# ------------------------------------------------------------------
# Universal live raymarch consumes previous completed grid.
# ------------------------------------------------------------------
p='engine/volume_render/src/UniversalVolumeRendererBase.inc'
rep(p,
'''[[vk::binding(6, 0)]]\nStructuredBuffer<GpuRadianceLevelInfo> g_radianceLevels : register(t6);\n\n[[vk::binding(7, 0)]]''',
'''[[vk::binding(6, 0)]]\nStructuredBuffer<GpuRadianceLevelInfo> g_radianceLevels : register(t6);\n[[vk::binding(7, 0)]]\nStructuredBuffer<uint4> g_particleLightGrid : register(t7);\n\n[[vk::binding(8, 0)]]''')
rep(p,'[[vk::binding(8, 0)]]\n[[vk::combinedImageSampler]]\nTexture2D g_history;', '[[vk::binding(9, 0)]]\n[[vk::combinedImageSampler]]\nTexture2D g_history;')
rep(p,'[[vk::binding(8, 0)]]\n[[vk::combinedImageSampler]]\nSamplerState g_historySampler;', '[[vk::binding(9, 0)]]\n[[vk::combinedImageSampler]]\nSamplerState g_historySampler;')
# particle grid helper before ShadowTransmittance
helper='''\nfloat4 SampleParticleLightGrid(float3 worldPosition)\n{\n    const uint4 meta = g_particleLightGrid[1];\n    const float3 origin = float3(asfloat(meta.x),asfloat(meta.y),asfloat(meta.z));\n    const float cellSize = asfloat(meta.w);\n    if (!(cellSize > 0.0)) return 0.0;\n    const int3 cell = int3(floor((worldPosition-origin)/cellSize));\n    const int resolution = 32;\n    if (any(cell < 0) || any(cell >= int3(resolution,resolution,resolution))) return 0.0;\n    const uint index = 2u + (uint(cell.z)*32u + uint(cell.y))*32u + uint(cell.x);\n    const uint4 packed = g_particleLightGrid[index];\n    return float4(float(packed.x)/4096.0, float3(packed.y,packed.z,packed.w)/1024.0);\n}\n\nfloat ParticleGridTransmittance(float3 start,float3 direction,float maximumDistance)\n{\n    if (maximumDistance <= 1.0e-4) return 1.0;\n    const float3 d=normalize(direction);\n    const float distance=min(maximumDistance,192.0);\n    const float stepLength=max(distance/6.0,8.0);\n    float optical=0.0;\n    [unroll] for(uint i=1u;i<=6u;++i)\n    {\n        const float t=min(stepLength*float(i),distance);\n        optical += SampleParticleLightGrid(start+d*t).x * 0.18;\n    }\n    return exp(-min(optical,20.0));\n}\n\n'''
ins_after(p,
'''float LocalLightScale(\n    GpuLocalLight light,\n    float distanceMeters,\n    float3 sampleToLight)\n{''', '')
# insert helper after LocalLightScale function by exact tail before ShadowTransmittance
rep(p,
'''        angular /\n        1361.0;\n}\n\nfloat ShadowTransmittance(''',
'''        angular /\n        1361.0;\n}\n'''+helper+'''float ShadowTransmittance(''')
rep(p,
'''            const float stellarShadow =\n                ShadowTransmittance(\n                    worldPosition +\n                        stellarDirection *\n                            0.01,\n                    stellarDirection,\n                    1.0e20,\n                    p,\n                    cellSize,\n                    tileEdge);''',
'''            const float stellarShadow =\n                ShadowTransmittance(\n                    worldPosition +\n                        stellarDirection *\n                            0.01,\n                    stellarDirection,\n                    1.0e20,\n                    p,\n                    cellSize,\n                    tileEdge) *\n                ParticleGridTransmittance(\n                    worldPosition + stellarDirection * 0.01,\n                    stellarDirection,\n                    192.0);''')
rep(p,
'''            incident +=\n                ambient *\n                0.07957747154594767;''',
'''            incident +=\n                ambient *\n                0.07957747154594767;\n\n            // Previous completed M38 particle grid contributes low-frequency\n            // emissive radiance without downloading particle state.\n            incident += SampleParticleLightGrid(worldPosition).yzw;''')
rep(p,
'''                const float shadow =\n                    ShadowTransmittance(\n                        worldPosition +\n                            sampleToLight *\n                                0.01,\n                        sampleToLight,\n                        lightDistance,\n                        p,\n                        cellSize,\n                        tileEdge);''',
'''                const float shadow =\n                    ShadowTransmittance(\n                        worldPosition +\n                            sampleToLight *\n                                0.01,\n                        sampleToLight,\n                        lightDistance,\n                        p,\n                        cellSize,\n                        tileEdge) *\n                    ParticleGridTransmittance(\n                        worldPosition + sampleToLight * 0.01,\n                        sampleToLight,\n                        lightDistance);''')
rep(p,
'''        std::unique_ptr<rhi::Buffer>\n            dummyRadianceLevels;''',
'''        std::unique_ptr<rhi::Buffer>\n            dummyRadianceLevels;\n        std::unique_ptr<rhi::Buffer>\n            dummyParticleLightGrid;''')
rep(p,
'''                kRaymarchPs,\n                0U,\n                7U,\n                2U);''',
'''                kRaymarchPs,\n                0U,\n                8U,\n                2U);''')
rep(p,
'''    const render_graph::BufferHandle radianceLevels,\n    const u32 radianceLevelCount,\n    const bool resetHistory)''',
'''    const render_graph::BufferHandle radianceLevels,\n    const u32 radianceLevelCount,\n    const render_graph::BufferHandle particleLightGrid,\n    const bool resetHistory)''')
# create dummy grid after dummy levels init
marker='''        presentation.\n            dummyRadianceLevels->Unmap();\n    }\n'''
addition='''\n    if (presentation.dummyParticleLightGrid == nullptr)\n    {\n        presentation.dummyParticleLightGrid = impl_->device->CreateBuffer({\n            .sizeBytes = 2U * sizeof(std::array<u32,4U>),\n            .usage = rhi::BufferUsage::Structured,\n            .memory = rhi::MemoryUsage::HostVisible,\n            .initialState = rhi::ResourceState::ShaderResource\n        });\n        std::memset(presentation.dummyParticleLightGrid->Map(),0,static_cast<std::size_t>(presentation.dummyParticleLightGrid->SizeBytes()));\n        presentation.dummyParticleLightGrid->Unmap();\n    }\n'''
ins_after(p,marker,addition)
# imports/effective grid
marker='''    const auto effectiveRadianceLevels =\n        radianceLevels.IsValid()\n            ? radianceLevels\n            : dummyRadianceLevels;\n'''
addition='''\n    const auto dummyParticleLightGrid = graph.ImportBuffer(\n        std::string(prefix) + ".DummyParticleLightGrid",\n        *presentation.dummyParticleLightGrid,\n        rhi::ResourceState::ShaderResource);\n    const auto effectiveParticleLightGrid =\n        particleLightGrid.IsValid()\n            ? particleLightGrid\n            : dummyParticleLightGrid;\n'''
ins_after(p,marker,addition)
rep(p,
'''            {effectiveRadianceCells,rhi::ResourceState::ShaderResource,render_graph::Access::Read},\n            {effectiveRadianceLevels,rhi::ResourceState::ShaderResource,render_graph::Access::Read}\n''',
'''            {effectiveRadianceCells,rhi::ResourceState::ShaderResource,render_graph::Access::Read},\n            {effectiveRadianceLevels,rhi::ResourceState::ShaderResource,render_graph::Access::Read},\n            {effectiveParticleLightGrid,rhi::ResourceState::ShaderResource,render_graph::Access::Read}\n''')
rep(p,
'''         effectiveRadianceCells,\n         effectiveRadianceLevels,\n         width,''',
'''         effectiveRadianceCells,\n         effectiveRadianceLevels,\n         effectiveParticleLightGrid,\n         width,''')
rep(p,
'''            commands.SetGraphicsBuffer(\n                6U,\n                resources.Buffer(\n                    effectiveRadianceLevels));\n            commands.SetGraphicsTexture(''',
'''            commands.SetGraphicsBuffer(\n                6U,\n                resources.Buffer(\n                    effectiveRadianceLevels));\n            commands.SetGraphicsBuffer(\n                7U,\n                resources.Buffer(\n                    effectiveParticleLightGrid));\n            commands.SetGraphicsTexture(''')

# ------------------------------------------------------------------
# Studio imports previous completed GPU light cache for universal volumes.
# ------------------------------------------------------------------
p='engine/studio_ui/src/StudioViewportRenderer.cpp'
marker='''                    const lighting::DirectionalLight\n                        volumeStellarLight{\n'''
# insert handle before stellar light construction
insert='''                    render_graph::BufferHandle previousParticleLightGrid{};\n                    if (volumeParticleRenderer_.ParticleLightGridReady())\n                    {\n                        previousParticleLightGrid = graph.ImportBuffer(\n                            prefix + ".ParticleLightGridPrevious",\n                            volumeParticleRenderer_.ParticleLightGrid(),\n                            rhi::ResourceState::ShaderResource);\n                    }\n\n'''
t=load(p)
if t.count(marker)!=1: raise RuntimeError(f'{p}: stellar marker count {t.count(marker)}')
t=t.replace(marker,insert+marker,1); save(p,t)
rep(p,
'''                            sharedRadianceLevelsHandle,\n                            sharedRadianceLevelCount,\n                            view->Lighting().change !=''',
'''                            sharedRadianceLevelsHandle,\n                            sharedRadianceLevelCount,\n                            previousParticleLightGrid,\n                            view->Lighting().change !=''')

# ------------------------------------------------------------------
# Regression: current Draw API + grid layout/readiness contract.
# ------------------------------------------------------------------
p='engine/studio_ui/tests/VolumeParticleRenderBridgeTests.cpp'
rep(p,
'''        const render_view::CameraState& camera)\n    {\n        renderer.Draw(''',
'''        const render_view::CameraState& camera,\n        const lighting::DirectionalLight& stellar)\n    {\n        renderer.Draw(''')
rep(p,
'''            0U,\n            0x12345678ULL,\n            3.0F);''',
'''            0U,\n            0x12345678ULL,\n            stellar,\n            std::span<const lighting::ResolvedLocalLight>{},\n            3.0F);''')
rep(p,
'''    Check((1U + volume_render::VolumeParticleRenderer::ParticleLightGridResolution * volume_render::VolumeParticleRenderer::ParticleLightGridResolution * volume_render::VolumeParticleRenderer::ParticleLightGridResolution) == 32769U);''',
'''    Check((2U + volume_render::VolumeParticleRenderer::ParticleLightGridResolution * volume_render::VolumeParticleRenderer::ParticleLightGridResolution * volume_render::VolumeParticleRenderer::ParticleLightGridResolution) == 32770U);\n    static_assert(requires(const volume_render::VolumeParticleRenderer& renderer) { renderer.ParticleLightGridReady(); });''')

print('M38 shared particle light cache patch applied')
