from pathlib import Path

def rep(path, old, new):
    p=Path(path); t=p.read_text(encoding='utf-8'); c=t.count(old)
    if c!=1: raise RuntimeError(f'{path}: expected 1 match got {c}: {old[:120]!r}')
    p.write_text(t.replace(old,new,1),encoding='utf-8')

# Header: persistent splash state and GPU resources.
p='engine/volume_render/include/orbit/volume_render/VolumeParticleGpuState.hpp'
rep(p,
'''static_assert(sizeof(VolumeParticleGpuSplashEvent) == 48U);\n\nstruct VolumeParticleSimulationSettings''',
'''static_assert(sizeof(VolumeParticleGpuSplashEvent) == 48U);\n\nstruct VolumeParticleGpuSplashState\n{\n    math::Float3 positionMeters{};\n    f32 baseScaleMeters{0.0F};\n    math::Float3 normal{};\n    f32 expansionMetersPerSecond{0.0F};\n    math::Float3 tint{1.0F, 1.0F, 1.0F};\n    f32 impactSpeedMetersPerSecond{0.0F};\n    f32 ageSeconds{0.0F};\n    f32 lifetimeSeconds{0.0F};\n    u32 generation{0U};\n    u32 reserved{0U};\n};\nstatic_assert(sizeof(VolumeParticleGpuSplashState) == 64U);\n\nstruct VolumeParticleSimulationSettings''')
rep(p,
'''    static constexpr u32 MaximumSplashEventCount = 4096U;\n    static constexpr u32 ComputeBufferCount = 4U;''',
'''    static constexpr u32 MaximumSplashEventCount = 4096U;\n    static constexpr u32 MaximumPersistentSplashCount = 8192U;\n    static constexpr u32 ComputeBufferCount = 4U;''')
rep(p,
'''    u32 generation_{0U};\n    bool initialized_{false};\n    bool currentIsA_{true};''',
'''    u32 generation_{0U};\n    u32 splashGeneration_{0U};\n    bool initialized_{false};\n    bool currentIsA_{true};\n    bool splashCurrentIsA_{true};''')
rep(p,
'''    rhi::ResourceState splashEventState_{rhi::ResourceState::UnorderedAccess};\n    rhi::ResourceState splashCounterState_{rhi::ResourceState::CopyDestination};\n''',
'''    rhi::ResourceState splashEventState_{rhi::ResourceState::UnorderedAccess};\n    rhi::ResourceState splashCounterState_{rhi::ResourceState::CopyDestination};\n    rhi::ResourceState splashStateAState_{rhi::ResourceState::CopyDestination};\n    rhi::ResourceState splashStateBState_{rhi::ResourceState::CopyDestination};\n    rhi::ResourceState splashStateCounterState_{rhi::ResourceState::CopyDestination};\n''')
rep(p,
'''    std::unique_ptr<rhi::Buffer> splashEvents_;\n    std::unique_ptr<rhi::Buffer> splashCounter_;\n    std::unique_ptr<rhi::Buffer> zeroSplashCounterUpload_;\n''',
'''    std::unique_ptr<rhi::Buffer> splashEvents_;\n    std::unique_ptr<rhi::Buffer> splashCounter_;\n    std::unique_ptr<rhi::Buffer> zeroSplashCounterUpload_;\n    std::unique_ptr<rhi::Buffer> splashStateA_;\n    std::unique_ptr<rhi::Buffer> splashStateB_;\n    std::unique_ptr<rhi::Buffer> zeroSplashStateUpload_;\n    std::unique_ptr<rhi::Buffer> splashStateCounter_;\n    std::unique_ptr<rhi::Buffer> zeroSplashStateCounterUpload_;\n''')
rep(p,
'''    std::unique_ptr<rhi::ComputePipeline> simulationPipeline_;\n    std::unique_ptr<rhi::ComputePipeline> terrainCollisionPipeline_;''',
'''    std::unique_ptr<rhi::ComputePipeline> simulationPipeline_;\n    std::unique_ptr<rhi::ComputePipeline> terrainCollisionPipeline_;\n    std::unique_ptr<rhi::ComputePipeline> splashSimulationPipeline_;''')

# GPU state source: add persistent splash simulation shader.
p='engine/volume_render/src/VolumeParticleGpuState.cpp'
insert='''\nconstexpr const char* kSplashSimulationShader = R"(\nstruct SplashEvent { float3 positionMeters; float scaleMeters; float3 normal; float impactSpeedMetersPerSecond; float3 tint; uint generation; };\nstruct SplashState { float3 positionMeters; float baseScaleMeters; float3 normal; float expansionMetersPerSecond; float3 tint; float impactSpeedMetersPerSecond; float ageSeconds; float lifetimeSeconds; uint generation; uint reserved; };\n[[vk::binding(0,0)]] RWStructuredBuffer<SplashState> g_source : register(u0);\n[[vk::binding(1,0)]] RWStructuredBuffer<SplashState> g_destination : register(u1);\n[[vk::binding(2,0)]] RWStructuredBuffer<SplashEvent> g_events : register(u2);\n[[vk::binding(3,0)]] RWStructuredBuffer<uint> g_eventCounter : register(u3);\n[[vk::binding(4,0)]] RWStructuredBuffer<uint> g_stateCounter : register(u4);\nstruct Push { uint4 counts; float4 timing; };\n[[vk::push_constant]] Push g;\nvoid Append(SplashState s){ uint i=0u; InterlockedAdd(g_stateCounter[0],1u,i); if(i<g.counts.w){ s.generation=g.counts.y; g_destination[i]=s; } }\n[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID){\n uint i=id.x; float dt=max(g.timing.x,0.0); float3 originDelta=g.timing.yzw;\n if(i<g.counts.z){ SplashState s=g_source[i]; if(s.generation==g.counts.x && s.lifetimeSeconds>0.0){ s.ageSeconds+=dt; if(s.ageSeconds<s.lifetimeSeconds){ s.positionMeters+=originDelta; Append(s); } } }\n uint eventCount=min(g_eventCounter[0],4096u);\n if(i<eventCount){ SplashEvent e=g_events[i]; if(e.generation==g.counts.y){ SplashState s; s.positionMeters=e.positionMeters; s.baseScaleMeters=max(e.scaleMeters,0.01); s.normal=normalize(e.normal); s.expansionMetersPerSecond=max(0.35*e.impactSpeedMetersPerSecond,0.15*s.baseScaleMeters); s.tint=max(e.tint,0.0); s.impactSpeedMetersPerSecond=max(e.impactSpeedMetersPerSecond,0.0); s.ageSeconds=0.0; s.lifetimeSeconds=clamp(0.45+0.10*s.impactSpeedMetersPerSecond,0.45,1.75); s.generation=g.counts.y; s.reserved=0u; Append(s); } }\n}\n)";\n\n'''
rep(p,'[[nodiscard]] u32 Bits(const f32 value) noexcept\n{',insert+'[[nodiscard]] u32 Bits(const f32 value) noexcept\n{')

# Allocate persistent state buffers and compile pipeline after splash event counter init.
rep(p,
'''    {\n        std::byte* mapped = zeroSplashCounterUpload_->Map();\n        std::memset(mapped, 0, sizeof(u32));\n        zeroSplashCounterUpload_->Unmap();\n    }\n\n    spawnBuffers_.reserve(framesInFlight);''',
'''    {\n        std::byte* mapped = zeroSplashCounterUpload_->Map();\n        std::memset(mapped, 0, sizeof(u32));\n        zeroSplashCounterUpload_->Unmap();\n    }\n\n    const u64 splashStateBytes = static_cast<u64>(MaximumPersistentSplashCount) * sizeof(VolumeParticleGpuSplashState);\n    const rhi::BufferDesc splashStateDesc{.sizeBytes=splashStateBytes,.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::CopyDestination};\n    splashStateA_=device.CreateBuffer(splashStateDesc);\n    splashStateB_=device.CreateBuffer(splashStateDesc);\n    zeroSplashStateUpload_=device.CreateBuffer({.sizeBytes=splashStateBytes,.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::HostVisible,.initialState=rhi::ResourceState::CopySource});\n    { std::byte* mapped=zeroSplashStateUpload_->Map(); std::memset(mapped,0,static_cast<std::size_t>(splashStateBytes)); zeroSplashStateUpload_->Unmap(); }\n    splashStateCounter_=device.CreateBuffer({.sizeBytes=sizeof(u32),.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::CopyDestination});\n    zeroSplashStateCounterUpload_=device.CreateBuffer({.sizeBytes=sizeof(u32),.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::HostVisible,.initialState=rhi::ResourceState::CopySource});\n    { std::byte* mapped=zeroSplashStateCounterUpload_->Map(); std::memset(mapped,0,sizeof(u32)); zeroSplashStateCounterUpload_->Unmap(); }\n\n    spawnBuffers_.reserve(framesInFlight);''')
rep(p,
'''    terrainCollisionPipeline_ = device.CreateComputePipeline({\n        .computeShader = {\n            .data = terrainCollision.bytecode.data(),\n            .size = terrainCollision.bytecode.size()\n        },\n        .pushConstantDwords = 12U,\n        .shaderResourceBuffers = 4U,\n        .storageTextures = 0U,\n        .sampledTextures = 0U,\n        .accelerationStructures = 0U\n    });\n}''',
'''    terrainCollisionPipeline_ = device.CreateComputePipeline({\n        .computeShader = {\n            .data = terrainCollision.bytecode.data(),\n            .size = terrainCollision.bytecode.size()\n        },\n        .pushConstantDwords = 12U,\n        .shaderResourceBuffers = 4U,\n        .storageTextures = 0U,\n        .sampledTextures = 0U,\n        .accelerationStructures = 0U\n    });\n\n    const auto splashSimulation=compiler.Compile({.source=kSplashSimulationShader,.entryPoint="main",.stage=shader::Stage::Compute,.debug=false});\n    if(splashSimulation.bytecode.empty()) throw std::runtime_error("Orbit failed to compile the M38 persistent splash simulation shader.");\n    splashSimulationPipeline_=device.CreateComputePipeline({.computeShader={.data=splashSimulation.bytecode.data(),.size=splashSimulation.bytecode.size()},.pushConstantDwords=8U,.shaderResourceBuffers=5U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});\n}''')

# Initialize persistent splash buffers.
rep(p,
'''    commands.CopyBuffer(\n        *zeroCounterUpload_, 0U, *counter_, 0U, sizeof(u32));\n\n    TransitionState(''',
'''    commands.CopyBuffer(\n        *zeroCounterUpload_, 0U, *counter_, 0U, sizeof(u32));\n    TransitionState(commands,*splashStateA_,splashStateAState_,rhi::ResourceState::CopyDestination);\n    TransitionState(commands,*splashStateB_,splashStateBState_,rhi::ResourceState::CopyDestination);\n    TransitionState(commands,*splashStateCounter_,splashStateCounterState_,rhi::ResourceState::CopyDestination);\n    commands.CopyBuffer(*zeroSplashStateUpload_,0U,*splashStateA_,0U,splashStateA_->SizeBytes());\n    commands.CopyBuffer(*zeroSplashStateUpload_,0U,*splashStateB_,0U,splashStateB_->SizeBytes());\n    commands.CopyBuffer(*zeroSplashStateCounterUpload_,0U,*splashStateCounter_,0U,sizeof(u32));\n\n    TransitionState(''')
rep(p,
'''    currentIsA_ = true;\n    generation_ = 0U;\n    initialized_ = true;''',
'''    currentIsA_ = true;\n    splashCurrentIsA_ = true;\n    generation_ = 0U;\n    splashGeneration_ = 0U;\n    initialized_ = true;''')

# ApplyTerrainCollision: do not early return for no pages; clear event counter, generate events if pages, then advance persistent splash state.
rep(p,
'''    if (!initialized_ || generation_ == 0U || pages.empty())\n    {\n        return;\n    }''',
'''    if (!initialized_ || generation_ == 0U)\n    {\n        return;\n    }''')
rep(p,
'''    TransitionState(commands, *splashEvents_, splashEventState_, rhi::ResourceState::ShaderResource);\n}\n\nvoid VolumeParticleGpuState::BindForGraphics''',
'''    TransitionState(commands, *splashEvents_, splashEventState_, rhi::ResourceState::UnorderedAccess);\n\n    // Compact/age persistent splash state and append this generation's exact-once water-entry events.\n    rhi::Buffer& splashSource=splashCurrentIsA_?*splashStateA_:*splashStateB_;\n    rhi::Buffer& splashDestination=splashCurrentIsA_?*splashStateB_:*splashStateA_;\n    rhi::ResourceState& splashSourceState=splashCurrentIsA_?splashStateAState_:splashStateBState_;\n    rhi::ResourceState& splashDestinationState=splashCurrentIsA_?splashStateBState_:splashStateAState_;\n    TransitionState(commands,splashSource,splashSourceState,rhi::ResourceState::UnorderedAccess);\n    TransitionState(commands,splashDestination,splashDestinationState,rhi::ResourceState::UnorderedAccess);\n    TransitionState(commands,*splashStateCounter_,splashStateCounterState_,rhi::ResourceState::CopyDestination);\n    commands.CopyBuffer(*zeroSplashStateCounterUpload_,0U,*splashStateCounter_,0U,sizeof(u32));\n    TransitionState(commands,*splashStateCounter_,splashStateCounterState_,rhi::ResourceState::UnorderedAccess);\n    const u32 previousSplashGeneration=splashGeneration_; ++splashGeneration_; if(splashGeneration_==0U) splashGeneration_=1U;\n    const f32 splashDt=std::isfinite(deltaSeconds)?static_cast<f32>(std::clamp(deltaSeconds,0.0,1.0)):0.0F;\n    std::array<u32,8U> splashConstants{}; splashConstants[0]=previousSplashGeneration; splashConstants[1]=splashGeneration_; splashConstants[2]=MaximumPersistentSplashCount; splashConstants[3]=MaximumPersistentSplashCount; splashConstants[4]=Bits(splashDt);\n    // Splash state shares the same presentation frame; particle rebasing delta was already applied in Advance. New events are in the new frame, while old splash state needs the same delta.\n    splashConstants[5]=0U; splashConstants[6]=0U; splashConstants[7]=0U;\n    commands.SetComputePipeline(*splashSimulationPipeline_); commands.SetComputeConstants(splashConstants); commands.SetComputeBuffer(0U,splashSource); commands.SetComputeBuffer(1U,splashDestination); commands.SetComputeBuffer(2U,*splashEvents_); commands.SetComputeBuffer(3U,*splashCounter_); commands.SetComputeBuffer(4U,*splashStateCounter_);\n    commands.Dispatch((std::max(MaximumPersistentSplashCount,MaximumSplashEventCount)+63U)/64U,1U,1U); commands.UavBarrier(splashDestination); splashCurrentIsA_=!splashCurrentIsA_;\n    TransitionState(commands,splashDestination,splashDestinationState,rhi::ResourceState::ShaderResource);\n}\n\nvoid VolumeParticleGpuState::BindForGraphics''')

# Bind persistent splash state rather than one-frame events.
rep(p,
'''    TransitionState(commands, *splashEvents_, splashEventState_, rhi::ResourceState::ShaderResource);\n    commands.SetGraphicsBuffer(SplashGraphicsBufferSlot, *splashEvents_);''',
'''    rhi::Buffer& splashCurrent=splashCurrentIsA_?*splashStateA_:*splashStateB_;\n    rhi::ResourceState& splashState=splashCurrentIsA_?splashStateAState_:splashStateBState_;\n    TransitionState(commands,splashCurrent,splashState,rhi::ResourceState::ShaderResource);\n    commands.SetGraphicsBuffer(SplashGraphicsBufferSlot,splashCurrent);''')
rep(p,
'''    generation_ = 0U;\n    initialized_ = false;''',
'''    generation_ = 0U;\n    splashGeneration_ = 0U;\n    splashCurrentIsA_ = true;\n    initialized_ = false;''')

# Renderer splash shader reads persistent state and expands/fades.
p='engine/volume_render/src/VolumeParticleRenderer.cpp'
start='''struct SplashEvent { float3 positionMeters; float scaleMeters; float3 normal; float impactSpeedMetersPerSecond; float3 tint; uint generation; };\n[[vk::binding(3, 0)]] StructuredBuffer<SplashEvent> g_splashes : register(t3);'''
repl='''struct SplashState { float3 positionMeters; float baseScaleMeters; float3 normal; float expansionMetersPerSecond; float3 tint; float impactSpeedMetersPerSecond; float ageSeconds; float lifetimeSeconds; uint generation; uint reserved; };\n[[vk::binding(3, 0)]] StructuredBuffer<SplashState> g_splashes : register(t3);'''
rep(p,start,repl)
rep(p,
'''    uint eventIndex=vertexId/6u, cornerIndex=vertexId%6u; SplashEvent e=g_splashes[eventIndex]; VSOutput o;\n    if(e.generation!=asuint(g.viewport.w)||e.scaleMeters<=0.0){o.position=float4(2,2,1,1);o.uv=0;o.tint=0;o.impact=0;return o;}''',
'''    uint eventIndex=vertexId/6u, cornerIndex=vertexId%6u; SplashState e=g_splashes[eventIndex]; VSOutput o;\n    if(e.generation==0u||e.baseScaleMeters<=0.0||e.lifetimeSeconds<=0.0||e.ageSeconds>=e.lifetimeSeconds){o.position=float4(2,2,1,1);o.uv=0;o.tint=0;o.impact=0;return o;}''')
rep(p,
'''    float pixels=max(e.scaleMeters,0.01)/max(center.w*max(g.projection.y,0.001),0.001)*max(g.viewport.y,1.0)*0.5;\n    pixels=max(pixels,max(g.viewport.z,0.5)); o.position=center; o.position.xy+=corners[cornerIndex]*ndc*pixels*center.w;\n    o.uv=corners[cornerIndex]; o.tint=max(e.tint,0.0); o.impact=max(e.impactSpeedMetersPerSecond,0.0); return o;''',
'''    float life=saturate(1.0-e.ageSeconds/max(e.lifetimeSeconds,0.001));\n    float radius=max(e.baseScaleMeters+e.expansionMetersPerSecond*e.ageSeconds,0.01);\n    float pixels=radius/max(center.w*max(g.projection.y,0.001),0.001)*max(g.viewport.y,1.0)*0.5;\n    pixels=max(pixels,max(g.viewport.z,0.5)); o.position=center; o.position.xy+=corners[cornerIndex]*ndc*pixels*center.w;\n    o.uv=corners[cornerIndex]; o.tint=max(e.tint,0.0)*life; o.impact=max(e.impactSpeedMetersPerSecond,0.0)*life; return o;''')
rep(p,'commands.Draw(VolumeParticleGpuState::MaximumSplashEventCount * 6U);','commands.Draw(VolumeParticleGpuState::MaximumPersistentSplashCount * 6U);')

# Regression constants.
p='engine/studio_ui/tests/VolumeParticleRenderBridgeTests.cpp'
rep(p,
'''    Check(volume_render::VolumeParticleGpuState::MaximumSplashEventCount == 4096U);\n    Check(volume_render::VolumeParticleGpuState::SplashGraphicsBufferSlot == 3U);''',
'''    Check(volume_render::VolumeParticleGpuState::MaximumSplashEventCount == 4096U);\n    Check(volume_render::VolumeParticleGpuState::MaximumPersistentSplashCount == 8192U);\n    Check(sizeof(volume_render::VolumeParticleGpuSplashState) == 64U);\n    Check(volume_render::VolumeParticleGpuState::SplashGraphicsBufferSlot == 3U);''')

print('M38 persistent splash patch applied')
