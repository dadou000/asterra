from pathlib import Path

def rep(path, old, new, count=1):
    p=Path(path); t=p.read_text(encoding='utf-8'); c=t.count(old)
    if c!=count: raise RuntimeError(f'{path}: expected {count} got {c}: {old[:120]!r}')
    p.write_text(t.replace(old,new,count),encoding='utf-8')

# ---- Particle renderer: frame-slot light grids and frame-space metadata ----
p='engine/volume_render/include/orbit/volume_render/VolumeParticleRenderer.hpp'
rep(p,'    [[nodiscard]] rhi::Buffer& ParticleLightGrid() noexcept;','    [[nodiscard]] rhi::Buffer& ParticleLightGrid(u32 frameIndex);')
rep(p,
'''    std::unique_ptr<rhi::ComputePipeline> particleLightGridPipeline_;
    std::unique_ptr<rhi::Buffer> particleLightGrid_;
    std::unique_ptr<rhi::Buffer> zeroParticleLightGridUpload_;
    rhi::ResourceState particleLightGridState_{rhi::ResourceState::CopyDestination};''',
'''    std::unique_ptr<rhi::ComputePipeline> particleLightGridPipeline_;
    std::vector<std::unique_ptr<rhi::Buffer>> particleLightGrids_;
    std::vector<std::unique_ptr<rhi::Buffer>> zeroParticleLightGridUploads_;
    std::vector<rhi::ResourceState> particleLightGridStates_;''')

p='engine/volume_render/src/VolumeParticleRenderer.cpp'
# Compute shader: relative bin origin + absolute/frame metadata origin.
rep(p,
'''struct Push { float4 originCell; uint4 params; }; [[vk::push_constant]] Push g;
[numthreads(64,1,1)] void main(uint3 tid:SV_DispatchThreadID){ uint i=tid.x; if(i==0u) g_grid[0]=uint4(asuint(g.originCell.x),asuint(g.originCell.y),asuint(g.originCell.z),asuint(g.originCell.w)); if(i>=65536u) return; Particle p=g_particles[i]; if(p.generation!=g.params.x||p.lifetimeSeconds<=0.0||p.ageSeconds>=p.lifetimeSeconds) return; float3 q=(p.positionMeters-g.originCell.xyz)/max(g.originCell.w,1e-4);''',
'''struct Push { float4 binOriginCell; float4 frameOriginCell; uint4 params; }; [[vk::push_constant]] Push g;
[numthreads(64,1,1)] void main(uint3 tid:SV_DispatchThreadID){ uint i=tid.x; if(i==0u) g_grid[0]=uint4(asuint(g.frameOriginCell.x),asuint(g.frameOriginCell.y),asuint(g.frameOriginCell.z),asuint(g.frameOriginCell.w)); if(i>=65536u) return; Particle p=g_particles[i]; if(p.generation!=g.params.x||p.lifetimeSeconds<=0.0||p.ageSeconds>=p.lifetimeSeconds) return; float3 q=(p.positionMeters-g.binOriginCell.xyz)/max(g.binOriginCell.w,1e-4);''')
rep(p,'.pushConstantDwords=8U,.shaderResourceBuffers=2U,','.pushConstantDwords=12U,.shaderResourceBuffers=2U,')
# Allocate a grid per frame slot.
rep(p,
'''    particleLightGrid_=device.CreateBuffer({.sizeBytes=gridBytes,.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::CopyDestination});
    zeroParticleLightGridUpload_=device.CreateBuffer({.sizeBytes=gridBytes,.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::HostVisible,.initialState=rhi::ResourceState::CopySource});
    {auto* m=zeroParticleLightGridUpload_->Map();std::memset(m,0,static_cast<std::size_t>(gridBytes));zeroParticleLightGridUpload_->Unmap();}''',
'''    particleLightGrids_.reserve(framesInFlight_);
    zeroParticleLightGridUploads_.reserve(framesInFlight_);
    particleLightGridStates_.assign(framesInFlight_,rhi::ResourceState::CopyDestination);
    for(u32 slot=0U;slot<framesInFlight_;++slot){
        particleLightGrids_.push_back(device.CreateBuffer({.sizeBytes=gridBytes,.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::CopyDestination}));
        zeroParticleLightGridUploads_.push_back(device.CreateBuffer({.sizeBytes=gridBytes,.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::HostVisible,.initialState=rhi::ResourceState::CopySource}));
        auto* m=zeroParticleLightGridUploads_.back()->Map();std::memset(m,0,static_cast<std::size_t>(gridBytes));zeroParticleLightGridUploads_.back()->Unmap();
    }''')
# Draw grid build: absolute metadata from camera frame-space, relative binning remains precision-safe.
rep(p,
'''    const math::Float3 lightGridOrigin{
        static_cast<f32>(cameraPositionRelativeToPresentationOriginMeters.x)-lightGridHalfExtent,
        static_cast<f32>(cameraPositionRelativeToPresentationOriginMeters.y)-lightGridHalfExtent,
        static_cast<f32>(cameraPositionRelativeToPresentationOriginMeters.z)-lightGridHalfExtent};
    if(particleLightGridState_!=rhi::ResourceState::CopyDestination){commands.Transition(*particleLightGrid_,particleLightGridState_,rhi::ResourceState::CopyDestination);particleLightGridState_=rhi::ResourceState::CopyDestination;}
    commands.CopyBuffer(*zeroParticleLightGridUpload_,0U,*particleLightGrid_,0U,particleLightGrid_->SizeBytes());
    commands.Transition(*particleLightGrid_,particleLightGridState_,rhi::ResourceState::UnorderedAccess); particleLightGridState_=rhi::ResourceState::UnorderedAccess;
    std::array<u32,8U> lightGridConstants{bits(lightGridOrigin.x),bits(lightGridOrigin.y),bits(lightGridOrigin.z),bits(lightGridCellMeters),state_.Generation(),ParticleLightGridResolution,0U,0U};
    commands.SetComputePipeline(*particleLightGridPipeline_); commands.SetComputeConstants(lightGridConstants); commands.SetComputeBuffer(0U,state_.CurrentBuffer()); commands.SetComputeBuffer(1U,*particleLightGrid_); commands.Dispatch((VolumeParticleGpuState::MaximumParticleCount+63U)/64U,1U,1U); commands.UavBarrier(*particleLightGrid_);
    commands.Transition(*particleLightGrid_,particleLightGridState_,rhi::ResourceState::ShaderResource); particleLightGridState_=rhi::ResourceState::ShaderResource;''',
'''    const math::Float3 lightGridBinOrigin{
        static_cast<f32>(cameraPositionRelativeToPresentationOriginMeters.x)-lightGridHalfExtent,
        static_cast<f32>(cameraPositionRelativeToPresentationOriginMeters.y)-lightGridHalfExtent,
        static_cast<f32>(cameraPositionRelativeToPresentationOriginMeters.z)-lightGridHalfExtent};
    const math::Float3 lightGridFrameOrigin{
        static_cast<f32>(camera.localPositionMeters.x)-lightGridHalfExtent,
        static_cast<f32>(camera.localPositionMeters.y)-lightGridHalfExtent,
        static_cast<f32>(camera.localPositionMeters.z)-lightGridHalfExtent};
    auto& particleLightGrid=*particleLightGrids_.at(frameIndex);
    auto& particleLightGridState=particleLightGridStates_.at(frameIndex);
    if(particleLightGridState!=rhi::ResourceState::CopyDestination){commands.Transition(particleLightGrid,particleLightGridState,rhi::ResourceState::CopyDestination);particleLightGridState=rhi::ResourceState::CopyDestination;}
    commands.CopyBuffer(*zeroParticleLightGridUploads_.at(frameIndex),0U,particleLightGrid,0U,particleLightGrid.SizeBytes());
    commands.Transition(particleLightGrid,particleLightGridState,rhi::ResourceState::UnorderedAccess); particleLightGridState=rhi::ResourceState::UnorderedAccess;
    std::array<u32,12U> lightGridConstants{bits(lightGridBinOrigin.x),bits(lightGridBinOrigin.y),bits(lightGridBinOrigin.z),bits(lightGridCellMeters),bits(lightGridFrameOrigin.x),bits(lightGridFrameOrigin.y),bits(lightGridFrameOrigin.z),bits(lightGridCellMeters),state_.Generation(),ParticleLightGridResolution,0U,0U};
    commands.SetComputePipeline(*particleLightGridPipeline_); commands.SetComputeConstants(lightGridConstants); commands.SetComputeBuffer(0U,state_.CurrentBuffer()); commands.SetComputeBuffer(1U,particleLightGrid); commands.Dispatch((VolumeParticleGpuState::MaximumParticleCount+63U)/64U,1U,1U); commands.UavBarrier(particleLightGrid);
    commands.Transition(particleLightGrid,particleLightGridState,rhi::ResourceState::ShaderResource); particleLightGridState=rhi::ResourceState::ShaderResource;''')
rep(p,'commands.SetGraphicsBuffer(9U,*particleLightGrid_);','commands.SetGraphicsBuffer(9U,particleLightGrid);',3)
rep(p,
'''    particleLightGridState_=rhi::ResourceState::CopyDestination;
}''',
'''    std::fill(particleLightGridStates_.begin(),particleLightGridStates_.end(),rhi::ResourceState::CopyDestination);
}''')
rep(p,
'''rhi::Buffer& VolumeParticleRenderer::ParticleLightGrid() noexcept
{
    return *particleLightGrid_;
}''',
'''rhi::Buffer& VolumeParticleRenderer::ParticleLightGrid(const u32 frameIndex)
{
    if(frameIndex>=particleLightGrids_.size()) throw std::out_of_range("Orbit M38 particle light-grid frame index exceeds frames in flight.");
    return *particleLightGrids_[frameIndex];
}''')

# ---- Universal volume API: optional previous particle-light grid ----
p='engine/volume_render/include/orbit/volume_render/UniversalVolumeRenderer.hpp'
rep(p,
'''        render_graph::BufferHandle radianceLevels,
        u32 radianceLevelCount,
        bool resetHistory);''',
'''        render_graph::BufferHandle radianceLevels,
        u32 radianceLevelCount,
        rhi::Buffer* particleLightGrid,
        bool resetHistory);''',3)

p='engine/volume_render/src/VolumeRepresentationPass.cpp'
rep(p,
'''    const render_graph::BufferHandle radianceLevels,
    const u32 radianceLevelCount,
    const bool resetHistory)''',
'''    const render_graph::BufferHandle radianceLevels,
    const u32 radianceLevelCount,
    rhi::Buffer* const particleLightGrid,
    const bool resetHistory)''')
rep(p,
'''        radianceLevels,
        radianceLevelCount,
        resetHistory);''',
'''        radianceLevels,
        radianceLevelCount,
        particleLightGrid,
        resetHistory);''')

# ---- Universal volume backend: sample particle extinction + emission ----
p='engine/volume_render/src/UniversalVolumeRendererBase.inc'
# shader binding and helper
rep(p,
'''[[vk::binding(6, 0)]]
StructuredBuffer<GpuRadianceLevelInfo> g_radianceLevels : register(t6);

[[vk::binding(7, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth;
[[vk::binding(7, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler;

[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_history;
[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_historySampler;''',
'''[[vk::binding(6, 0)]]
StructuredBuffer<GpuRadianceLevelInfo> g_radianceLevels : register(t6);
[[vk::binding(7, 0)]]
StructuredBuffer<uint4> g_particleLightGrid : register(t7);

[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth;
[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler;

[[vk::binding(9, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_history;
[[vk::binding(9, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_historySampler;''')
helper='''
float4 SampleParticleLightGrid(float3 framePosition)
{
    const uint4 meta = g_particleLightGrid[0];
    const float3 origin = float3(asfloat(meta.x), asfloat(meta.y), asfloat(meta.z));
    const float cell = asfloat(meta.w);
    if (!(cell > 0.0)) return 0.0;
    const int3 c = int3(floor((framePosition - origin) / cell));
    const int r = 32;
    if (any(c < 0) || any(c >= int3(r,r,r))) return 0.0;
    const uint idx = 1u + (uint(c.z) * r + uint(c.y)) * r + uint(c.x);
    const uint4 packed = g_particleLightGrid[idx];
    return float4(float(packed.x) / 4096.0, float3(packed.yzw) / 1024.0);
}

float ParticleGridTransmittance(float3 start, float3 direction, float maximumDistance)
{
    if (maximumDistance <= 1.0e-4) return 1.0;
    const float3 d = normalize(direction);
    float optical = 0.0;
    const float stepLength = max(maximumDistance / 6.0, 8.0);
    [unroll]
    for (uint i=1u;i<=6u;++i)
    {
        const float distance = min(stepLength * float(i), maximumDistance);
        optical += SampleParticleLightGrid(start + d * distance).x * 0.18;
    }
    return exp(-min(optical,20.0));
}

'''
rep(p,'float HenyeyGreenstein(',helper+'float HenyeyGreenstein(')
# apply stellar and local attenuation + emission field injection
rep(p,
'''            const float stellarShadow =
                ShadowTransmittance(
                    worldPosition +
                        stellarDirection *
                            0.01,
                    stellarDirection,
                    1.0e20,
                    p,
                    cellSize,
                    tileEdge);''',
'''            const float stellarShadow =
                ShadowTransmittance(
                    worldPosition +
                        stellarDirection *
                            0.01,
                    stellarDirection,
                    1.0e20,
                    p,
                    cellSize,
                    tileEdge) *
                ParticleGridTransmittance(
                    worldPosition,
                    stellarDirection,
                    192.0);''')
rep(p,
'''                const float shadow =
                    ShadowTransmittance(
                        worldPosition +
                            sampleToLight *
                                0.01,
                        sampleToLight,
                        lightDistance,
                        p,
                        cellSize,
                        tileEdge);''',
'''                const float shadow =
                    ShadowTransmittance(
                        worldPosition +
                            sampleToLight *
                                0.01,
                        sampleToLight,
                        lightDistance,
                        p,
                        cellSize,
                        tileEdge) *
                    ParticleGridTransmittance(
                        worldPosition,
                        sampleToLight,
                        min(lightDistance,192.0));''')
rep(p,
'''            scattering =
                sigmaS *
                incident;''',
'''            incident += SampleParticleLightGrid(worldPosition).yzw;
            scattering =
                sigmaS *
                incident;''')
# backend pipeline count and dummy buffer
rep(p,
'''        std::unique_ptr<rhi::Buffer>
            dummyRadianceLevels;''',
'''        std::unique_ptr<rhi::Buffer>
            dummyRadianceLevels;
        std::unique_ptr<rhi::Buffer>
            dummyParticleLightGrid;''')
rep(p,
'''                0U,
                7U,
                2U);''',
'''                0U,
                8U,
                2U);''')
# signature
rep(p,
'''    const render_graph::BufferHandle radianceLevels,
    const u32 radianceLevelCount,
    const bool resetHistory)''',
'''    const render_graph::BufferHandle radianceLevels,
    const u32 radianceLevelCount,
    rhi::Buffer* const particleLightGrid,
    const bool resetHistory)''')
# create dummy after dummy radiance levels block
needle='''        presentation.
            dummyRadianceLevels->Unmap();
    }

    const f32 aspect ='''
insert='''        presentation.
            dummyRadianceLevels->Unmap();
    }

    if (presentation.dummyParticleLightGrid == nullptr)
    {
        constexpr u64 gridBytes = (1ULL + 32ULL * 32ULL * 32ULL) * sizeof(std::array<u32,4U>);
        presentation.dummyParticleLightGrid = impl_->device->CreateBuffer({
            .sizeBytes = gridBytes,
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::ShaderResource
        });
        std::memset(presentation.dummyParticleLightGrid->Map(),0,static_cast<std::size_t>(gridBytes));
        presentation.dummyParticleLightGrid->Unmap();
    }

    const f32 aspect ='''
rep(p,needle,insert)
# import effective grid
needle='''    const auto effectiveRadianceLevels =
        radianceLevels.IsValid()
            ? radianceLevels
            : dummyRadianceLevels;

    auto emission ='''
insert='''    const auto effectiveRadianceLevels =
        radianceLevels.IsValid()
            ? radianceLevels
            : dummyRadianceLevels;
    const auto particleLightGridHandle = graph.ImportBuffer(
        std::string(prefix) + ".ParticleLightGrid",
        particleLightGrid != nullptr ? *particleLightGrid : *presentation.dummyParticleLightGrid,
        rhi::ResourceState::ShaderResource);

    auto emission ='''
rep(p,needle,insert)
# pass use + capture + bind
rep(p,
'''            {effectiveRadianceLevels,rhi::ResourceState::ShaderResource,render_graph::Access::Read}
        },''',
'''            {effectiveRadianceLevels,rhi::ResourceState::ShaderResource,render_graph::Access::Read},
            {particleLightGridHandle,rhi::ResourceState::ShaderResource,render_graph::Access::Read}
        },''')
rep(p,
'''         effectiveRadianceLevels,
         width,''',
'''         effectiveRadianceLevels,
         particleLightGridHandle,
         width,''')
rep(p,
'''            commands.SetGraphicsBuffer(
                6U,
                resources.Buffer(
                    effectiveRadianceLevels));
            commands.SetGraphicsTexture(''',
'''            commands.SetGraphicsBuffer(
                6U,
                resources.Buffer(
                    effectiveRadianceLevels));
            commands.SetGraphicsBuffer(
                7U,
                resources.Buffer(
                    particleLightGridHandle));
            commands.SetGraphicsTexture(''')

# ---- Studio: pass previous slot grid into universal volume ----
p='engine/studio_ui/src/StudioViewportRenderer.cpp'
rep(p,
'''                            sharedRadianceLevelsHandle,
                            sharedRadianceLevelCount,
                            view->Lighting().change !=''',
'''                            sharedRadianceLevelsHandle,
                            sharedRadianceLevelCount,
                            particleOutputGeneration_ > 0U
                                ? &volumeParticleRenderer_.ParticleLightGrid(frameIndex % framesInFlight_)
                                : nullptr,
                            view->Lighting().change !=''')

# ---- stale regression Draw signature + accessor contract ----
p='engine/studio_ui/tests/VolumeParticleRenderBridgeTests.cpp'
rep(p,
'''            0U,
            0x12345678ULL,
            3.0F);''',
'''            0U,
            0x12345678ULL,
            lighting::DirectionalLight{},
            std::span<const lighting::ResolvedLocalLight>{},
            3.0F);''')
rep(p,
'''    Check(volume_render::VolumeParticleRenderer::ParticleLightGridResolution == 32U);''',
'''    Check(volume_render::VolumeParticleRenderer::ParticleLightGridResolution == 32U);
    static_assert(requires(volume_render::VolumeParticleRenderer& renderer){ renderer.ParticleLightGrid(0U); });''')

print('M38 shared particle light grid integration applied')
