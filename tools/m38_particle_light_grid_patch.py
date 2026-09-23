from pathlib import Path

def rep(path,old,new):
 p=Path(path); t=p.read_text(encoding='utf-8'); c=t.count(old)
 if c!=1: raise RuntimeError(f'{path}: expected 1 match got {c}: {old[:100]!r}')
 p.write_text(t.replace(old,new,1),encoding='utf-8')

# Header: add GPU light-grid contract/resources.
p='engine/volume_render/include/orbit/volume_render/VolumeParticleRenderer.hpp'
rep(p,'    static constexpr u32 MaximumLocalLightCount = 64U;\n','    static constexpr u32 MaximumLocalLightCount = 64U;\n    static constexpr u32 ParticleLightGridResolution = 32U;\n')
rep(p,'    [[nodiscard]] u32 SubmittedSpawnCount() const noexcept;\n','    [[nodiscard]] u32 SubmittedSpawnCount() const noexcept;\n    [[nodiscard]] rhi::Buffer& ParticleLightGrid() noexcept;\n')
rep(p,'    std::unique_ptr<rhi::GraphicsPipeline> oitCompositePipeline_;\n','    std::unique_ptr<rhi::GraphicsPipeline> oitCompositePipeline_;\n    std::unique_ptr<rhi::ComputePipeline> particleLightGridPipeline_;\n    std::unique_ptr<rhi::Buffer> particleLightGrid_;\n    std::unique_ptr<rhi::Buffer> zeroParticleLightGridUpload_;\n    rhi::ResourceState particleLightGridState_{rhi::ResourceState::CopyDestination};\n')

# Source includes.
p='engine/volume_render/src/VolumeParticleRenderer.cpp'
t=Path(p).read_text(encoding='utf-8')
if '#include <cstring>' not in t: t=t.replace('#include <cmath>\n','#include <cmath>\n#include <cstring>\n')
Path(p).write_text(t,encoding='utf-8')

# Add grid shader before OIT composite shader.
rep(p,'constexpr const char* kOitCompositeVertexShader = R"(\n',r'''constexpr const char* kParticleLightGridShader = R"(
struct Particle { float3 positionMeters; float authority; float3 velocityMetersPerSecond; float density; float emission; float ageSeconds; float lifetimeSeconds; float linearDragPerSecond; float radiusMeters; float emissionScale; float gravityScale; float restitution; float3 baseColor; uint behaviorFlags; float3 emissionColor; uint generation; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; float waterDensityRatio; float waterDragPerSecond; float waterBuoyancyScale; float waterSplashScale; uint4 bodyIdentity; };
[[vk::binding(0,0)]] StructuredBuffer<Particle> g_particles;
[[vk::binding(1,0)]] RWStructuredBuffer<uint4> g_grid;
struct Push { float4 originCell; uint4 params; }; [[vk::push_constant]] Push g;
[numthreads(64,1,1)] void main(uint3 tid:SV_DispatchThreadID){ uint i=tid.x; if(i>=65536u) return; Particle p=g_particles[i]; if(p.generation!=g.params.x||p.lifetimeSeconds<=0.0||p.ageSeconds>=p.lifetimeSeconds) return; float3 q=(p.positionMeters-g.originCell.xyz)/max(g.originCell.w,1e-4); int3 c=int3(floor(q)); uint r=g.params.y; if(any(c<0)||any(c>=int3(r,r,r))) return; uint idx=(uint(c.z)*r+uint(c.y))*r+uint(c.x); float life=saturate(1.0-p.ageSeconds/max(p.lifetimeSeconds,1e-4)); float optical=max(p.density,0.0)*max(p.authority,0.0)*life*max(p.radiusMeters,0.01); float3 e=max(p.emissionColor,0.0)*max(p.emission,0.0)*max(p.emissionScale,0.0)*life; uint opticalQ=(uint)min(optical*4096.0,16777215.0); uint3 emissionQ=(uint3)min(e*1024.0,16777215.0); InterlockedAdd(g_grid[idx].x,opticalQ); InterlockedAdd(g_grid[idx].y,emissionQ.x); InterlockedAdd(g_grid[idx].z,emissionQ.y); InterlockedAdd(g_grid[idx].w,emissionQ.z); }
)";

constexpr const char* kOitCompositeVertexShader = R"(
''')

# Compile + allocate after other shaders compile.
rep(p,'    const auto oitCompositePixel=compiler.Compile({.source=kOitCompositePixelShader,.entryPoint="main",.stage=shader::Stage::Pixel,.debug=false});\n', '    const auto oitCompositePixel=compiler.Compile({.source=kOitCompositePixelShader,.entryPoint="main",.stage=shader::Stage::Pixel,.debug=false});\n    const auto particleLightGrid=compiler.Compile({.source=kParticleLightGridShader,.entryPoint="main",.stage=shader::Stage::Compute,.debug=false});\n')
rep(p,'    if (vertex.bytecode.empty() || pixel.bytecode.empty() || splashVertex.bytecode.empty() || splashPixel.bytecode.empty() || dropletVertex.bytecode.empty() || dropletPixel.bytecode.empty() || oitCompositeVertex.bytecode.empty() || oitTemporalPixel.bytecode.empty() || oitCompositePixel.bytecode.empty())\n', '    if (vertex.bytecode.empty() || pixel.bytecode.empty() || splashVertex.bytecode.empty() || splashPixel.bytecode.empty() || dropletVertex.bytecode.empty() || dropletPixel.bytecode.empty() || oitCompositeVertex.bytecode.empty() || oitTemporalPixel.bytecode.empty() || oitCompositePixel.bytecode.empty() || particleLightGrid.bytecode.empty())\n')
rep(p,'    oitCompositePipeline_=device.CreateGraphicsPipeline({.vertexShader={.data=oitCompositeVertex.bytecode.data(),.size=oitCompositeVertex.bytecode.size()},.pixelShader={.data=oitCompositePixel.bytecode.data(),.size=oitCompositePixel.bytecode.size()},.vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=0U,.shaderResourceBuffers=0U,.sampledTextures=1U,.topology=rhi::PrimitiveTopology::TriangleList,.fillMode=rhi::FillMode::Solid,.cullMode=rhi::CullMode::None,.blendMode=rhi::BlendMode::Alpha,.depthCompare=rhi::DepthCompare::LessEqual,.depthTest=false,.depthWrite=false,.colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float},.colorAttachmentCount=1U});\n', '    oitCompositePipeline_=device.CreateGraphicsPipeline({.vertexShader={.data=oitCompositeVertex.bytecode.data(),.size=oitCompositeVertex.bytecode.size()},.pixelShader={.data=oitCompositePixel.bytecode.data(),.size=oitCompositePixel.bytecode.size()},.vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=0U,.shaderResourceBuffers=0U,.sampledTextures=1U,.topology=rhi::PrimitiveTopology::TriangleList,.fillMode=rhi::FillMode::Solid,.cullMode=rhi::CullMode::None,.blendMode=rhi::BlendMode::Alpha,.depthCompare=rhi::DepthCompare::LessEqual,.depthTest=false,.depthWrite=false,.colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float},.colorAttachmentCount=1U});\n    particleLightGridPipeline_=device.CreateComputePipeline({.computeShader={.data=particleLightGrid.bytecode.data(),.size=particleLightGrid.bytecode.size()},.pushConstantDwords=8U,.shaderResourceBuffers=2U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});\n    constexpr u64 gridBytes=static_cast<u64>(ParticleLightGridResolution)*ParticleLightGridResolution*ParticleLightGridResolution*sizeof(std::array<u32,4U>);\n    particleLightGrid_=device.CreateBuffer({.sizeBytes=gridBytes,.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::CopyDestination});\n    zeroParticleLightGridUpload_=device.CreateBuffer({.sizeBytes=gridBytes,.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::HostVisible,.initialState=rhi::ResourceState::CopySource});\n    {auto* m=zeroParticleLightGridUpload_->Map();std::memset(m,0,static_cast<std::size_t>(gridBytes));zeroParticleLightGridUpload_->Unmap();}\n')

# Build grid in Draw after visible-list build. 512m half extent, 32^3 => 32m cells.
rep(p,'    auto& oit=EnsureOitTargets(width,height,frameIndex,temporalHistoryKey);\n',r'''    // Live-particle light authority: camera-centered 32^3 grid, 32 m cells.
    constexpr f32 lightGridCellMeters=32.0F;
    constexpr f32 lightGridHalfExtent=lightGridCellMeters*static_cast<f32>(ParticleLightGridResolution)*0.5F;
    const math::Float3 lightGridOrigin{
        static_cast<f32>(cameraPositionRelativeToPresentationOriginMeters.x)-lightGridHalfExtent,
        static_cast<f32>(cameraPositionRelativeToPresentationOriginMeters.y)-lightGridHalfExtent,
        static_cast<f32>(cameraPositionRelativeToPresentationOriginMeters.z)-lightGridHalfExtent};
    if(particleLightGridState_!=rhi::ResourceState::CopyDestination){commands.Transition(*particleLightGrid_,particleLightGridState_,rhi::ResourceState::CopyDestination);particleLightGridState_=rhi::ResourceState::CopyDestination;}
    commands.CopyBuffer(*zeroParticleLightGridUpload_,0U,*particleLightGrid_,0U,particleLightGrid_->SizeBytes());
    commands.Transition(*particleLightGrid_,particleLightGridState_,rhi::ResourceState::UnorderedAccess); particleLightGridState_=rhi::ResourceState::UnorderedAccess;
    std::array<u32,8U> lightGridConstants{bits(lightGridOrigin.x),bits(lightGridOrigin.y),bits(lightGridOrigin.z),bits(lightGridCellMeters),state_.Generation(),ParticleLightGridResolution,0U,0U};
    commands.SetComputePipeline(*particleLightGridPipeline_); commands.SetComputeConstants(lightGridConstants); commands.SetComputeBuffer(0U,state_.CurrentBuffer()); commands.SetComputeBuffer(1U,*particleLightGrid_); commands.Dispatch((VolumeParticleGpuState::MaximumParticleCount+63U)/64U,1U,1U); commands.UavBarrier(*particleLightGrid_);
    commands.Transition(*particleLightGrid_,particleLightGridState_,rhi::ResourceState::ShaderResource); particleLightGridState_=rhi::ResourceState::ShaderResource;

    auto& oit=EnsureOitTargets(width,height,frameIndex,temporalHistoryKey);
''')

# Expose accessor and reset state.
rep(p,'u32 VolumeParticleRenderer::SubmittedSpawnCount() const noexcept\n{\n    return state_.SubmittedSpawnCount();\n}\n', 'u32 VolumeParticleRenderer::SubmittedSpawnCount() const noexcept\n{\n    return state_.SubmittedSpawnCount();\n}\n\nrhi::Buffer& VolumeParticleRenderer::ParticleLightGrid() noexcept\n{\n    return *particleLightGrid_;\n}\n')
rep(p,'    oitTargets_.clear();\n','    oitTargets_.clear();\n    particleLightGridState_=rhi::ResourceState::CopyDestination;\n')

# Regression contract.
p='engine/studio_ui/tests/VolumeParticleRenderBridgeTests.cpp'
rep(p,'    Check(volume_render::VolumeParticleRenderer::MaximumLocalLightCount == 64U);\n','    Check(volume_render::VolumeParticleRenderer::MaximumLocalLightCount == 64U);\n    Check(volume_render::VolumeParticleRenderer::ParticleLightGridResolution == 32U);\n')
print('M38 particle light grid patch applied')
