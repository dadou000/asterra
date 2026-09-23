from pathlib import Path


def replace_once(path, old, new):
    p=Path(path); text=p.read_text(encoding='utf-8'); n=text.count(old)
    if n!=1: raise RuntimeError(f'{path}: expected 1 match, got {n}: {old[:120]!r}')
    p.write_text(text.replace(old,new,1),encoding='utf-8')

# GPU state contract: bounded splash event stream, no readback.
path='engine/volume_render/include/orbit/volume_render/VolumeParticleGpuState.hpp'
replace_once(path,
'''static_assert(sizeof(VolumeParticleGpuStateRecord) == 160U);\n\nstruct VolumeParticleSimulationSettings\n''',
'''static_assert(sizeof(VolumeParticleGpuStateRecord) == 160U);\n\nstruct VolumeParticleGpuSplashEvent\n{\n    math::Float3 positionMeters{};\n    f32 scaleMeters{0.0F};\n    math::Float3 normal{};\n    f32 impactSpeedMetersPerSecond{0.0F};\n    math::Float3 tint{1.0F, 1.0F, 1.0F};\n    u32 generation{0U};\n};\nstatic_assert(sizeof(VolumeParticleGpuSplashEvent) == 48U);\n\nstruct VolumeParticleSimulationSettings\n''')
replace_once(path,
'''    static constexpr u32 MaximumParticleCount = 65536U;\n    static constexpr u32 ComputeBufferCount = 4U;\n    static constexpr u32 GraphicsBufferSlot = 2U;\n''',
'''    static constexpr u32 MaximumParticleCount = 65536U;\n    static constexpr u32 MaximumSplashEventCount = 4096U;\n    static constexpr u32 ComputeBufferCount = 4U;\n    static constexpr u32 GraphicsBufferSlot = 2U;\n    static constexpr u32 SplashGraphicsBufferSlot = 3U;\n''')
replace_once(path,
'''    rhi::ResourceState counterState_{rhi::ResourceState::CopyDestination};\n\n    std::unique_ptr<rhi::Buffer> stateA_;\n''',
'''    rhi::ResourceState counterState_{rhi::ResourceState::CopyDestination};\n    rhi::ResourceState splashEventState_{rhi::ResourceState::UnorderedAccess};\n    rhi::ResourceState splashCounterState_{rhi::ResourceState::CopyDestination};\n\n    std::unique_ptr<rhi::Buffer> stateA_;\n''')
replace_once(path,
'''    std::unique_ptr<rhi::Buffer> zeroCounterUpload_;\n    std::vector<std::unique_ptr<rhi::Buffer>> spawnBuffers_;\n''',
'''    std::unique_ptr<rhi::Buffer> zeroCounterUpload_;\n    std::unique_ptr<rhi::Buffer> splashEvents_;\n    std::unique_ptr<rhi::Buffer> splashCounter_;\n    std::unique_ptr<rhi::Buffer> zeroSplashCounterUpload_;\n    std::vector<std::unique_ptr<rhi::Buffer>> spawnBuffers_;\n''')

# Terrain shader: append exact-once splash event from pending bit.
path='engine/volume_render/src/VolumeParticleGpuState.cpp'
replace_once(path,
'''[[vk::binding(0, 0)]]\nRWStructuredBuffer<Particle> g_particles : register(u0);\n[[vk::binding(1, 0)]]\nByteAddressBuffer g_physicalPage : register(t1);\n''',
'''struct SplashEvent\n{\n    float3 positionMeters;\n    float scaleMeters;\n    float3 normal;\n    float impactSpeedMetersPerSecond;\n    float3 tint;\n    uint generation;\n};\n\n[[vk::binding(0, 0)]]\nRWStructuredBuffer<Particle> g_particles : register(u0);\n[[vk::binding(1, 0)]]\nByteAddressBuffer g_physicalPage : register(t1);\n[[vk::binding(2, 0)]]\nRWStructuredBuffer<SplashEvent> g_splashes : register(u2);\n[[vk::binding(3, 0)]]\nRWStructuredBuffer<uint> g_splashCounter : register(u3);\n''')
replace_once(path,
'''bool SameBody(uint4 a, uint4 b)\n{\n    return all(a == b);\n}\n\n[numthreads(64, 1, 1)]\n''',
'''bool SameBody(uint4 a, uint4 b)\n{\n    return all(a == b);\n}\n\nvoid EmitSplash(Particle particle, float3 positionMeters, float3 normal, float impactSpeed)\n{\n    const uint kWaterEntryPending = 1u << 30u;\n    if ((particle.behaviorFlags & kWaterEntryPending) == 0u) return;\n\n    uint eventIndex = 0u;\n    InterlockedAdd(g_splashCounter[0], 1u, eventIndex);\n    if (eventIndex < 4096u)\n    {\n        SplashEvent event;\n        event.positionMeters = positionMeters;\n        event.scaleMeters = max(particle.radiusMeters * max(particle.waterSplashScale, 0.0), 0.01);\n        event.normal = normal;\n        event.impactSpeedMetersPerSecond = max(impactSpeed, 0.0);\n        event.tint = lerp(float3(1.0, 1.0, 1.0), max(particle.baseColor, 0.0), 0.15);\n        event.generation = g.meta.y;\n        g_splashes[eventIndex] = event;\n    }\n}\n\n[numthreads(64, 1, 1)]\n''')
replace_once(path,
'''    if (nowSubmerged) particle.behaviorFlags |= kWaterSubmerged;\n    else particle.behaviorFlags &= ~kWaterSubmerged;\n\n    if (killForWater)\n''',
'''    if (nowSubmerged) particle.behaviorFlags |= kWaterSubmerged;\n    else particle.behaviorFlags &= ~kWaterSubmerged;\n\n    // Consume the pending entry event exactly once on the first/highest-detail\n    // resident page that covers this particle. Later overlapping LOD pages see\n    // the cleared bit and cannot duplicate the splash.\n    if ((particle.behaviorFlags & kWaterEntryPending) != 0u)\n    {\n        float waterRadius = terrainRadius + standingWaterDepthMeters;\n        float3 waterContact = particle.bodyCenterMeters + direction * waterRadius;\n        float impactSpeed = max(-dot(particle.velocityMetersPerSecond, normal), 0.0);\n        EmitSplash(particle, waterContact, normal, impactSpeed);\n        particle.behaviorFlags &= ~kWaterEntryPending;\n    }\n\n    if (killForWater)\n''')
# Allocate splash resources after particle counter upload.
replace_once(path,
'''    {\n        std::byte* mapped = zeroCounterUpload_->Map();\n        std::memset(mapped, 0, sizeof(u32));\n        zeroCounterUpload_->Unmap();\n    }\n\n    spawnBuffers_.reserve(framesInFlight);\n''',
'''    {\n        std::byte* mapped = zeroCounterUpload_->Map();\n        std::memset(mapped, 0, sizeof(u32));\n        zeroCounterUpload_->Unmap();\n    }\n\n    splashEvents_ = device.CreateBuffer({\n        .sizeBytes = static_cast<u64>(MaximumSplashEventCount) * sizeof(VolumeParticleGpuSplashEvent),\n        .usage = rhi::BufferUsage::Structured,\n        .memory = rhi::MemoryUsage::GpuOnly,\n        .initialState = rhi::ResourceState::UnorderedAccess\n    });\n    splashCounter_ = device.CreateBuffer({\n        .sizeBytes = sizeof(u32),\n        .usage = rhi::BufferUsage::Structured,\n        .memory = rhi::MemoryUsage::GpuOnly,\n        .initialState = rhi::ResourceState::CopyDestination\n    });\n    zeroSplashCounterUpload_ = device.CreateBuffer({\n        .sizeBytes = sizeof(u32),\n        .usage = rhi::BufferUsage::Structured,\n        .memory = rhi::MemoryUsage::HostVisible,\n        .initialState = rhi::ResourceState::CopySource\n    });\n    {\n        std::byte* mapped = zeroSplashCounterUpload_->Map();\n        std::memset(mapped, 0, sizeof(u32));\n        zeroSplashCounterUpload_->Unmap();\n    }\n\n    spawnBuffers_.reserve(framesInFlight);\n''')
replace_once(path,
'''        .pushConstantDwords = 12U,\n        .shaderResourceBuffers = 2U,\n        .storageTextures = 0U,\n''',
'''        .pushConstantDwords = 12U,\n        .shaderResourceBuffers = 4U,\n        .storageTextures = 0U,\n''')
# Reset splash counter and make event stream writable once per generation.
replace_once(path,
'''    TransitionState(\n        commands,\n        current,\n        currentState,\n        rhi::ResourceState::UnorderedAccess);\n\n    commands.SetComputePipeline(*terrainCollisionPipeline_);\n''',
'''    TransitionState(\n        commands,\n        current,\n        currentState,\n        rhi::ResourceState::UnorderedAccess);\n    TransitionState(commands, *splashEvents_, splashEventState_, rhi::ResourceState::UnorderedAccess);\n    TransitionState(commands, *splashCounter_, splashCounterState_, rhi::ResourceState::CopyDestination);\n    commands.CopyBuffer(*zeroSplashCounterUpload_, 0U, *splashCounter_, 0U, sizeof(u32));\n    TransitionState(commands, *splashCounter_, splashCounterState_, rhi::ResourceState::UnorderedAccess);\n\n    commands.SetComputePipeline(*terrainCollisionPipeline_);\n''')
replace_once(path,
'''        commands.SetComputeBuffer(0U, current);\n        commands.SetComputeBuffer(1U, *page.samples);\n        commands.Dispatch((MaximumParticleCount + 63U) / 64U, 1U, 1U);\n        commands.UavBarrier(current);\n''',
'''        commands.SetComputeBuffer(0U, current);\n        commands.SetComputeBuffer(1U, *page.samples);\n        commands.SetComputeBuffer(2U, *splashEvents_);\n        commands.SetComputeBuffer(3U, *splashCounter_);\n        commands.Dispatch((MaximumParticleCount + 63U) / 64U, 1U, 1U);\n        commands.UavBarrier(current);\n        commands.UavBarrier(*splashEvents_);\n''')
replace_once(path,
'''    TransitionState(\n        commands,\n        current,\n        currentState,\n        rhi::ResourceState::ShaderResource);\n}\n\nvoid VolumeParticleGpuState::BindForGraphics(\n''',
'''    TransitionState(\n        commands,\n        current,\n        currentState,\n        rhi::ResourceState::ShaderResource);\n    TransitionState(commands, *splashEvents_, splashEventState_, rhi::ResourceState::ShaderResource);\n}\n\nvoid VolumeParticleGpuState::BindForGraphics(\n''')
replace_once(path,
'''    commands.SetGraphicsBuffer(GraphicsBufferSlot, current);\n}\n''',
'''    commands.SetGraphicsBuffer(GraphicsBufferSlot, current);\n    TransitionState(commands, *splashEvents_, splashEventState_, rhi::ResourceState::ShaderResource);\n    commands.SetGraphicsBuffer(SplashGraphicsBufferSlot, *splashEvents_);\n}\n''')

# Renderer: second fixed-capacity ring billboard pass from GPU splash stream.
path='engine/volume_render/include/orbit/volume_render/VolumeParticleRenderer.hpp'
replace_once(path,
'''    VolumeParticleGpuState state_;\n    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;\n''',
'''    VolumeParticleGpuState state_;\n    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;\n    std::unique_ptr<rhi::GraphicsPipeline> splashPipeline_;\n''')

path='engine/volume_render/src/VolumeParticleRenderer.cpp'
insert='''\nconstexpr const char* kSplashVertexShader = R"(\nstruct SplashEvent { float3 positionMeters; float scaleMeters; float3 normal; float impactSpeedMetersPerSecond; float3 tint; uint generation; };\n[[vk::binding(3, 0)]] StructuredBuffer<SplashEvent> g_splashes : register(t3);\nstruct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; };\n[[vk::push_constant]] Push g;\nstruct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float impact:TEXCOORD2; };\nfloat4 Project(float3 relative) {\n    float3 forward=normalize(g.forward.xyz); float3 requestedUp=normalize(g.up.xyz);\n    float3 right=normalize(cross(forward,requestedUp)); float3 cameraUp=normalize(cross(right,forward));\n    float z=dot(relative,forward); if(z<=g.projection.z||z>=g.projection.w) return float4(2,2,1,1);\n    float x=dot(relative,right), y=dot(relative,cameraUp);\n    return float4(x/(max(g.projection.x,0.001)*max(g.projection.y,0.001)),-y/max(g.projection.y,0.001),z*0.5,z);\n}\nVSOutput main(uint vertexId:SV_VertexID) {\n    static const float2 corners[6]={float2(-1,-1),float2(1,-1),float2(1,1),float2(-1,-1),float2(1,1),float2(-1,1)};\n    uint eventIndex=vertexId/6u, cornerIndex=vertexId%6u; SplashEvent e=g_splashes[eventIndex]; VSOutput o;\n    if(e.generation!=asuint(g.viewport.w)||e.scaleMeters<=0.0){o.position=float4(2,2,1,1);o.uv=0;o.tint=0;o.impact=0;return o;}\n    float4 center=Project(e.positionMeters-g.camera.xyz); if(center.w<=0.0){o.position=center;o.uv=0;o.tint=0;o.impact=0;return o;}\n    float2 ndc=float2(2.0/max(g.viewport.x,1.0),2.0/max(g.viewport.y,1.0));\n    float pixels=max(e.scaleMeters,0.01)/max(center.w*max(g.projection.y,0.001),0.001)*max(g.viewport.y,1.0)*0.5;\n    pixels=max(pixels,max(g.viewport.z,0.5)); o.position=center; o.position.xy+=corners[cornerIndex]*ndc*pixels*center.w;\n    o.uv=corners[cornerIndex]; o.tint=max(e.tint,0.0); o.impact=max(e.impactSpeedMetersPerSecond,0.0); return o;\n}\n)";\nconstexpr const char* kSplashPixelShader = R"(\nstruct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float impact:TEXCOORD2; };\nfloat4 main(VSOutput i):SV_Target0 {\n    float r=length(i.uv); if(r>=1.0||r<0.42) discard;\n    float ring=(1.0-smoothstep(0.42,0.62,r))*smoothstep(0.42,0.52,r);\n    float impact=saturate(i.impact/8.0); float alpha=ring*(0.35+0.55*impact);\n    float3 foam=lerp(float3(0.72,0.84,0.90),float3(1.0,1.0,1.0),impact)*i.tint;\n    return float4(foam,alpha);\n}\n)";\n'''
replace_once(path,'constexpr const char* kPixelShader = R"(',insert+'\nconstexpr const char* kPixelShader = R"(')
# Compile + create splash pipeline after particle pipeline creation.
needle='''    pipeline_ = device.CreateGraphicsPipeline({\n        .vertexShader = {\n            .data = vertex.bytecode.data(),\n'''
# Easier: insert compile before if and pipeline after existing pipeline closing using unique constructor end.
replace_once(path,
'''    if (vertex.bytecode.empty() || pixel.bytecode.empty())\n    {\n''',
'''    const auto splashVertex = compiler.Compile({.source=kSplashVertexShader,.entryPoint="main",.stage=shader::Stage::Vertex,.debug=false});\n    const auto splashPixel = compiler.Compile({.source=kSplashPixelShader,.entryPoint="main",.stage=shader::Stage::Pixel,.debug=false});\n    if (vertex.bytecode.empty() || pixel.bytecode.empty() || splashVertex.bytecode.empty() || splashPixel.bytecode.empty())\n    {\n''')
replace_once(path,
'''        .colorAttachmentCount = 1U\n    });\n}\n\nvoid VolumeParticleRenderer::SetSpawns(\n''',
'''        .colorAttachmentCount = 1U\n    });\n\n    splashPipeline_ = device.CreateGraphicsPipeline({\n        .vertexShader={.data=splashVertex.bytecode.data(),.size=splashVertex.bytecode.size()},\n        .pixelShader={.data=splashPixel.bytecode.data(),.size=splashPixel.bytecode.size()},\n        .vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=20U,.shaderResourceBuffers=4U,.sampledTextures=0U,\n        .topology=rhi::PrimitiveTopology::TriangleList,.fillMode=rhi::FillMode::Solid,.cullMode=rhi::CullMode::None,\n        .blendMode=rhi::BlendMode::Alpha,.depthCompare=rhi::DepthCompare::LessEqual,.depthTest=true,.depthWrite=false,\n        .colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float},.colorAttachmentCount=1U\n    });\n}\n\nvoid VolumeParticleRenderer::SetSpawns(\n''')
replace_once(path,
'''    state_.BindForGraphics(commands);\n    commands.Draw(VolumeParticleGpuState::MaximumParticleCount * 6U);\n}\n''',
'''    state_.BindForGraphics(commands);\n    commands.Draw(VolumeParticleGpuState::MaximumParticleCount * 6U);\n    commands.SetGraphicsPipeline(*splashPipeline_);\n    commands.SetGraphicsConstants(constants);\n    state_.BindForGraphics(commands);\n    commands.Draw(VolumeParticleGpuState::MaximumSplashEventCount * 6U);\n}\n''')

print('M38 GPU splash stream patch applied')
