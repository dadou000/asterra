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
old_header='''constexpr const char* kGatherCs = R"(
[[vk::binding(0, 0)]]
RWTexture2D<float4> g_currentIndirect : register(u0);
[[vk::binding(1, 0)]]
RWTexture2D<float4> g_currentMeta : register(u1);

[[vk::binding(2, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_sceneColor : register(t2);
[[vk::binding(2, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sceneSampler : register(s2);

[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_baseRoughness : register(t3);
[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_baseSampler : register(s3);

[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_normalMetallic : register(t4);
[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_normalSampler : register(s4);

[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_emissionClass : register(t5);
[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_emissionSampler : register(s5);

[[vk::binding(6, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth : register(t6);
[[vk::binding(6, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler : register(s6);

[[vk::binding(7, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_previousIndirect : register(t7);
[[vk::binding(7, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_previousIndirectSampler : register(s7);

[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_previousMeta : register(t8);
[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_previousMetaSampler : register(s8);
'''
new_header='''constexpr const char* kGatherCs = R"(
[[vk::binding(0, 0)]]
StructuredBuffer<uint4> g_particleLightGrid : register(t0);

[[vk::binding(1, 0)]]
RWTexture2D<float4> g_currentIndirect : register(u1);
[[vk::binding(2, 0)]]
RWTexture2D<float4> g_currentMeta : register(u2);

[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_sceneColor : register(t3);
[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sceneSampler : register(s3);

[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_baseRoughness : register(t4);
[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_baseSampler : register(s4);

[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_normalMetallic : register(t5);
[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_normalSampler : register(s5);

[[vk::binding(6, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_emissionClass : register(t6);
[[vk::binding(6, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_emissionSampler : register(s6);

[[vk::binding(7, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth : register(t7);
[[vk::binding(7, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler : register(s7);

[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_previousIndirect : register(t8);
[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_previousIndirectSampler : register(s8);

[[vk::binding(9, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_previousMeta : register(t9);
[[vk::binding(9, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_previousMetaSampler : register(s9);
'''
rep(p,old_header,new_header)

rep(p,
'''    float4 depthRangeRadius;
    float4 gatherTuning;
};''',
'''    float4 depthRangeRadius;
    float4 gatherTuning;
    float4 cameraFrameParticleGrid;
};''')

helper='''
float4 SampleParticleLightGrid(float3 framePosition)
{
    if (g.cameraFrameParticleGrid.w <= 0.0) return 0.0;
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
rep(p,
'''                    accumulated +=
                        radiance *
                        weight;''',
'''                    const float particleT =
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

rep(p,'#include <cmath>\n','#include <cmath>\n#include <cstring>\n')
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
'''    const auto combine =
        compiler.Compile({''',
'''    dummyParticleLightGrid_ = device.CreateBuffer({
        .sizeBytes = 2U * sizeof(std::array<u32,4U>),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource
    });
    std::memset(dummyParticleLightGrid_->Map(),0,static_cast<std::size_t>(dummyParticleLightGrid_->SizeBytes()));
    dummyParticleLightGrid_->Unmap();

    const auto combine =
        compiler.Compile({''')
rep(p,
'''    const LightingView& view,
    const bool historyCompatible,
    const ScreenSpaceFinalGatherSettings& settings)''',
'''    const LightingView& view,
    const bool historyCompatible,
    rhi::Buffer* const particleLightGrid,
    const ScreenSpaceFinalGatherSettings& settings)''')
rep(p,'    const std::array<u32, 20> fullConstants{','    const std::array<u32, 24> fullConstants{')
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

# ---- Studio ----
p='engine/studio_ui/src/StudioViewportRenderer.cpp'
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
