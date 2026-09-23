from pathlib import Path

def rep(path, old, new, count=1):
    p=Path(path); t=p.read_text(encoding='utf-8'); c=t.count(old)
    if c!=count: raise RuntimeError(f'{path}: expected {count} got {c}: {old[:120]!r}')
    p.write_text(t.replace(old,new,count),encoding='utf-8')

# ---- API ----
p='engine/lighting/include/orbit/lighting/ScreenSpaceFinalGather.hpp'
rep(p,
'''        const LightingView& view,
        bool historyCompatible,
        const ScreenSpaceFinalGatherSettings& settings = {});''',
'''        const LightingView& view,
        bool historyCompatible,
        rhi::Buffer* particleLightGrid = nullptr,
        const ScreenSpaceFinalGatherSettings& settings = {});''')
rep(p,
'''    std::unique_ptr<rhi::ComputePipeline> gatherPipeline_;
    std::unique_ptr<rhi::ComputePipeline> combinePipeline_;''',
'''    std::unique_ptr<rhi::ComputePipeline> gatherPipeline_;
    std::unique_ptr<rhi::ComputePipeline> combinePipeline_;
    std::unique_ptr<rhi::Buffer> dummyParticleLightGrid_;''')

# ---- Shader / renderer ----
p='engine/lighting/src/ScreenSpaceFinalGather.cpp'
# Add buffer binding 0 and shift storage/sample bindings by one.
rep(p,
'''constexpr const char* kGatherCs = R"(
[[vk::binding(0, 0)]]
RWTexture2D<float4> g_currentIndirect : register(u0);
[[vk::binding(1, 0)]]
RWTexture2D<float4> g_currentMeta : register(u1);

[[vk::binding(2, 0)]]''',
'''constexpr const char* kGatherCs = R"(
[[vk::binding(0, 0)]]
StructuredBuffer<uint4> g_particleLightGrid : register(t0);

[[vk::binding(1, 0)]]
RWTexture2D<float4> g_currentIndirect : register(u1);
[[vk::binding(2, 0)]]
RWTexture2D<float4> g_currentMeta : register(u2);

[[vk::binding(3, 0)]]''')
# Shift remaining sampled texture binding attributes, one by one.
for old,new in [(3,4),(4,5),(5,6),(6,7),(7,8),(8,9)]:
    rep(p,f'[[vk::binding({old}, 0)]]',f'[[vk::binding({new}, 0)]]',2)
# Extend constants with frame origin + enable flag.
rep(p,
'''    float4 depthRangeRadius;
    float4 gatherTuning;
};''',
'''    float4 depthRangeRadius;
    float4 gatherTuning;
    float4 cameraFrameParticleGrid;
};''')
# Add shared-grid helpers before Hash12.
helper='''
float4 SampleParticleLightGrid(float3 framePosition)
{
    if (g.cameraFrameParticleGrid.w <= 0.0)
    {
        return 0.0;
    }
    const uint4 meta = g_particleLightGrid[1];
    const float3 origin = float3(asfloat(meta.x),asfloat(meta.y),asfloat(meta.z));
    const float cellSize = asfloat(meta.w);
    if (!(cellSize > 0.0)) return 0.0;
    const int3 cell = int3(floor((framePosition-origin)/cellSize));
    if (any(cell < 0) || any(cell >= int3(32,32,32))) return 0.0;
    const uint index = 2u + (uint(cell.z)*32u + uint(cell.y))*32u + uint(cell.x);
    const uint4 packed = g_particleLightGrid[index];
    return float4(float(packed.x)/4096.0,float3(packed.yzw)/1024.0);
}

float ParticleGridTransmittance(float3 start,float3 direction,float distance)
{
    if (g.cameraFrameParticleGrid.w <= 0.0 || distance <= 1.0e-4) return 1.0;
    const float3 ray = normalize(direction);
    const float boundedDistance = min(distance,192.0);
    const float stepLength = max(boundedDistance/6.0,8.0);
    float optical = 0.0;
    [unroll] for(uint i=1u;i<=6u;++i)
    {
        const float t=min(stepLength*float(i),boundedDistance);
        optical += SampleParticleLightGrid(start+ray*t).x*0.18;
    }
    return exp(-min(optical,20.0));
}

float3 ParticleGridEmissionAlong(float3 start,float3 direction,float distance)
{
    if (g.cameraFrameParticleGrid.w <= 0.0 || distance <= 1.0e-4) return 0.0;
    const float3 ray=normalize(direction);
    const float boundedDistance=min(distance,192.0);
    float3 sum=0.0;
    [unroll] for(uint i=1u;i<=4u;++i)
    {
        const float t=boundedDistance*(float(i)-0.5)/4.0;
        sum += SampleParticleLightGrid(start+ray*t).yzw;
    }
    return sum*0.25;
}

'''
rep(p,'float Hash12(float2 p)\n{',helper+'float Hash12(float2 p)\n{')
# At source position, derive frame-space origin once.
rep(p,
'''    const float3 surfacePosition =
        ReconstructPosition(
            uv,
            depth);

    const float radius =''',
'''    const float3 surfacePosition =
        ReconstructPosition(
            uv,
            depth);
    const float3 surfaceFramePosition =
        surfacePosition +
        g.cameraFrameParticleGrid.xyz;

    const float radius =''')
# Modify resolved hit radiance to include M38 extinction/emission along ray.
rep(p,
'''                    const float3 radiance =
                        max(
                            g_sceneColor.SampleLevel(
                                g_sceneSampler,
                                hitUv,
                                0).rgb,
                            0.0);

                    accumulated +=
                        radiance *
                        weight;''',
'''                    const float3 radiance =
                        max(
                            g_sceneColor.SampleLevel(
                                g_sceneSampler,
                                hitUv,
                                0).rgb,
                            0.0);
                    const float particleT =
                        ParticleGridTransmittance(
                            surfaceFramePosition,
                            direction,
                            t);
                    const float3 particleEmission =
                        ParticleGridEmissionAlong(
                            surfaceFramePosition,
                            direction,
                            t);

                    accumulated +=
                        (radiance * particleT + particleEmission) *
                        weight;''')
# Pipeline descriptor counts and dummy grid.
rep(p,
'''            .pushConstantDwords = 20U,
            .shaderResourceBuffers = 0U,
            .storageTextures = 2U,
            .sampledTextures = 7U''',
'''            .pushConstantDwords = 24U,
            .shaderResourceBuffers = 1U,
            .storageTextures = 2U,
            .sampledTextures = 7U''')
rep(p,
'''    combinePipeline_ =
        device.CreateComputePipeline({''',
'''    dummyParticleLightGrid_ = device.CreateBuffer({
        .sizeBytes = 2U * sizeof(std::array<u32,4U>),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource
    });
    std::memset(dummyParticleLightGrid_->Map(),0,static_cast<std::size_t>(dummyParticleLightGrid_->SizeBytes()));
    dummyParticleLightGrid_->Unmap();

    combinePipeline_ =
        device.CreateComputePipeline({''')
# Need cstring for memset.
rep(p,'#include <cmath>\n','#include <cmath>\n#include <cstring>\n')
# Gather signature.
rep(p,
'''    const LightingView& view,
    const bool historyCompatible,
    const ScreenSpaceFinalGatherSettings& settings)''',
'''    const LightingView& view,
    const bool historyCompatible,
    rhi::Buffer* const particleLightGrid,
    const ScreenSpaceFinalGatherSettings& settings)''')
# constants 20 -> 24, append frame-space camera and enable.
rep(p,
'''    const std::array<u32, 20> fullConstants{''',
'''    const std::array<u32, 24> fullConstants{''')
rep(p,
'''        tuning[0],
        tuning[1],
        tuning[2],
        tuning[3]
    };''',
'''        tuning[0],
        tuning[1],
        tuning[2],
        tuning[3],

        bits(static_cast<f32>(view.cameraPositionInFrameMeters.x)),
        bits(static_cast<f32>(view.cameraPositionInFrameMeters.y)),
        bits(static_cast<f32>(view.cameraPositionInFrameMeters.z)),
        bits(particleLightGrid != nullptr ? 1.0F : 0.0F)
    };''')
# Bind buffer before storage textures.
rep(p,
'''    commands.SetComputeConstants(
        fullConstants);

    commands.SetComputeStorageTexture(''',
'''    commands.SetComputeConstants(
        fullConstants);
    commands.SetComputeBuffer(
        0U,
        particleLightGrid != nullptr
            ? *particleLightGrid
            : *dummyParticleLightGrid_);

    commands.SetComputeStorageTexture(''')

# ---- Studio call ----
p='engine/studio_ui/src/StudioViewportRenderer.cpp'
# Capture the already-resolved previous-frame grid pointer in gather lambda.
rep(p,
'''                 gatherSettings,
                 lightingTimestamps,''',
'''                 gatherSettings,
                 particleLightGridForDirect,
                 lightingTimestamps,''')
rep(p,
'''                        lightingView,
                        historyCompatible,
                        gatherSettings);''',
'''                        lightingView,
                        historyCompatible,
                        particleLightGridForDirect,
                        gatherSettings);''')

print('M38 particle-aware screen-space final gather applied')
