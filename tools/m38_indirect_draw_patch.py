from pathlib import Path

def rep(path, old, new):
    p=Path(path); t=p.read_text(encoding='utf-8'); c=t.count(old)
    if c!=1: raise RuntimeError(f'{path}: expected 1 match, got {c}: {old[:140]!r}')
    p.write_text(t.replace(old,new,1),encoding='utf-8')

# RHI: explicit indirect resource state/usage and command.
p='engine/rhi/include/orbit/rhi/Resource.hpp'
rep(p,'    CopySource,\n    CopyDestination\n};','    CopySource,\n    CopyDestination,\n    IndirectArgument\n};')
rep(p,'    Constant,\n    Structured\n};','    Constant,\n    Structured,\n    // Storage-writable GPU draw arguments consumed by DrawIndirect.\n    Indirect\n};')

p='engine/rhi/include/orbit/rhi/Command.hpp'
rep(p,
'''    virtual void Draw(\n        u32 vertexCount,\n        u32 firstVertex = 0) = 0;\n''',
'''    virtual void Draw(\n        u32 vertexCount,\n        u32 firstVertex = 0) = 0;\n\n    // Non-indexed GPU-driven draw. `argumentBuffer` contains the native\n    // four-u32 draw layout: vertexCount, instanceCount, firstVertex,\n    // firstInstance. The buffer must be in ResourceState::IndirectArgument.\n    virtual void DrawIndirect(\n        Buffer& argumentBuffer,\n        u64 argumentOffsetBytes = 0) = 0;\n''')

p='engine/rhi/vulkan/src/VulkanObjects.hpp'
rep(p,
'''    void Draw(\n        u32 vertexCount,\n        u32 firstVertex) override;\n''',
'''    void Draw(\n        u32 vertexCount,\n        u32 firstVertex) override;\n\n    void DrawIndirect(\n        Buffer& argumentBuffer,\n        u64 argumentOffsetBytes) override;\n''')

p='engine/rhi/vulkan/src/VulkanResources.cpp'
rep(p,
'''    case BufferUsage::Structured:\n        // Bound via SetGraphicsBuffer -> vkCmdPushDescriptorSetKHR as\n        // a storage buffer (see VulkanCommands.cpp); this is the\n        // Vulkan analogue of the D3D12 root-descriptor SRV binding\n        // every ByteAddressBuffer/StructuredBuffer in this codebase\n        // uses today.\n        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;\n        break;\n''',
'''    case BufferUsage::Structured:\n        // Bound via SetGraphicsBuffer -> vkCmdPushDescriptorSetKHR as\n        // a storage buffer (see VulkanCommands.cpp); this is the\n        // Vulkan analogue of the D3D12 root-descriptor SRV binding\n        // every ByteAddressBuffer/StructuredBuffer in this codebase\n        // uses today.\n        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;\n        break;\n    case BufferUsage::Indirect:\n        // Compute writes the arguments through a storage descriptor and the\n        // graphics queue later consumes the same allocation via vkCmdDrawIndirect.\n        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;\n        break;\n''')

p='engine/rhi/vulkan/src/VulkanCommands.cpp'
rep(p,
'''    case ResourceState::CopyDestination:\n        return {\n            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,\n            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,\n            VK_ACCESS_2_TRANSFER_WRITE_BIT};\n\n    case ResourceState::VertexOrConstantBuffer:\n''',
'''    case ResourceState::CopyDestination:\n        return {\n            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,\n            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,\n            VK_ACCESS_2_TRANSFER_WRITE_BIT};\n\n    case ResourceState::IndirectArgument:\n        // Buffer-only state.\n        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};\n\n    case ResourceState::VertexOrConstantBuffer:\n''')
rep(p,
'''    case ResourceState::CopyDestination:\n        return {\n            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,\n            VK_ACCESS_2_TRANSFER_WRITE_BIT};\n    }\n''',
'''    case ResourceState::CopyDestination:\n        return {\n            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,\n            VK_ACCESS_2_TRANSFER_WRITE_BIT};\n\n    case ResourceState::IndirectArgument:\n        return {\n            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,\n            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};\n    }\n''')
rep(p,
'''void VulkanCommandList::Draw(\n    const u32 vertexCount,\n    const u32 firstVertex)\n{\n    vkCmdDraw(nativeCommandList_, vertexCount, 1, firstVertex, 0);\n}\n''',
'''void VulkanCommandList::Draw(\n    const u32 vertexCount,\n    const u32 firstVertex)\n{\n    vkCmdDraw(nativeCommandList_, vertexCount, 1, firstVertex, 0);\n}\n\nvoid VulkanCommandList::DrawIndirect(\n    Buffer& argumentBuffer,\n    const u64 argumentOffsetBytes)\n{\n    auto* vulkanBuffer = dynamic_cast<VulkanBuffer*>(&argumentBuffer);\n    if (vulkanBuffer == nullptr)\n    {\n        throw std::runtime_error(\n            "Orbit Vulkan received an indirect argument buffer from another backend.");\n    }\n    if (argumentBuffer.Usage() != BufferUsage::Indirect)\n    {\n        throw std::invalid_argument(\n            "Orbit DrawIndirect requires BufferUsage::Indirect.");\n    }\n    if ((argumentOffsetBytes % 4U) != 0U ||\n        argumentOffsetBytes + sizeof(VkDrawIndirectCommand) > argumentBuffer.SizeBytes())\n    {\n        throw std::out_of_range(\n            "Orbit DrawIndirect argument range exceeds the supplied buffer.");\n    }\n    vkCmdDrawIndirect(\n        nativeCommandList_,\n        vulkanBuffer->Native(),\n        static_cast<VkDeviceSize>(argumentOffsetBytes),\n        1U,\n        sizeof(VkDrawIndirectCommand));\n}\n''')

# GPU particle state: active-index lists + draw args.
p='engine/volume_render/include/orbit/volume_render/VolumeParticleGpuState.hpp'
rep(p,
'''    static constexpr u32 DropletGraphicsBufferSlot = 4U;\n''',
'''    static constexpr u32 DropletGraphicsBufferSlot = 4U;\n    static constexpr u32 ParticleActiveIndexGraphicsBufferSlot = 5U;\n    static constexpr u32 SplashActiveIndexGraphicsBufferSlot = 6U;\n    static constexpr u32 DropletActiveIndexGraphicsBufferSlot = 7U;\n    static constexpr u64 ParticleIndirectOffsetBytes = 0U;\n    static constexpr u64 SplashIndirectOffsetBytes = 16U;\n    static constexpr u64 DropletIndirectOffsetBytes = 32U;\n''')
rep(p,
'''    [[nodiscard]] rhi::Buffer& CurrentBuffer() noexcept;\n''',
'''    [[nodiscard]] rhi::Buffer& CurrentBuffer() noexcept;\n    [[nodiscard]] rhi::Buffer& IndirectDrawArguments() noexcept;\n''')
rep(p,
'''    rhi::ResourceState dropletCounterState_{rhi::ResourceState::CopyDestination};\n''',
'''    rhi::ResourceState dropletCounterState_{rhi::ResourceState::CopyDestination};\n    rhi::ResourceState particleActiveIndicesState_{rhi::ResourceState::UnorderedAccess};\n    rhi::ResourceState splashActiveIndicesState_{rhi::ResourceState::UnorderedAccess};\n    rhi::ResourceState dropletActiveIndicesState_{rhi::ResourceState::UnorderedAccess};\n    rhi::ResourceState indirectDrawArgumentsState_{rhi::ResourceState::CopyDestination};\n''')
rep(p,
'''    std::unique_ptr<rhi::Buffer> zeroDropletCounterUpload_;\n    std::vector<std::unique_ptr<rhi::Buffer>> spawnBuffers_;\n''',
'''    std::unique_ptr<rhi::Buffer> zeroDropletCounterUpload_;\n    std::unique_ptr<rhi::Buffer> particleActiveIndices_;\n    std::unique_ptr<rhi::Buffer> splashActiveIndices_;\n    std::unique_ptr<rhi::Buffer> dropletActiveIndices_;\n    std::unique_ptr<rhi::Buffer> indirectDrawArguments_;\n    std::unique_ptr<rhi::Buffer> zeroIndirectDrawArgumentsUpload_;\n    std::vector<std::unique_ptr<rhi::Buffer>> spawnBuffers_;\n''')
rep(p,
'''    std::unique_ptr<rhi::ComputePipeline> dropletCollisionPipeline_;\n''',
'''    std::unique_ptr<rhi::ComputePipeline> dropletCollisionPipeline_;\n    std::unique_ptr<rhi::ComputePipeline> drawListPipeline_;\n''')

# Add draw-list compute shader before Bits().
p='engine/volume_render/src/VolumeParticleGpuState.cpp'
shader='''\nconstexpr const char* kDrawListShader = R"(\nstruct Particle { float3 positionMeters; float authority; float3 velocityMetersPerSecond; float density; float emission; float ageSeconds; float lifetimeSeconds; float linearDragPerSecond; float radiusMeters; float emissionScale; float gravityScale; float restitution; float3 baseColor; uint behaviorFlags; float3 emissionColor; uint generation; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; float waterDensityRatio; float waterDragPerSecond; float waterBuoyancyScale; float waterSplashScale; uint4 bodyIdentity; };\nstruct SplashState { float3 positionMeters; float baseScaleMeters; float3 normal; float expansionMetersPerSecond; float3 tint; float impactSpeedMetersPerSecond; float ageSeconds; float lifetimeSeconds; uint generation; uint reserved; };\nstruct Droplet { float3 positionMeters; float radiusMeters; float3 velocityMetersPerSecond; float ageSeconds; float3 tint; float lifetimeSeconds; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; uint4 bodyIdentity; uint generation; uint flags; uint sourceParticleGeneration; uint reserved1; };\n[[vk::binding(0,0)]] StructuredBuffer<Particle> g_particles : register(t0);\n[[vk::binding(1,0)]] StructuredBuffer<SplashState> g_splashes : register(t1);\n[[vk::binding(2,0)]] StructuredBuffer<Droplet> g_droplets : register(t2);\n[[vk::binding(3,0)]] RWStructuredBuffer<uint> g_particleIndices : register(u3);\n[[vk::binding(4,0)]] RWStructuredBuffer<uint> g_splashIndices : register(u4);\n[[vk::binding(5,0)]] RWStructuredBuffer<uint> g_dropletIndices : register(u5);\n[[vk::binding(6,0)]] RWByteAddressBuffer g_drawArgs : register(u6);\nstruct Push { uint4 generation; uint4 capacity; }; [[vk::push_constant]] Push g;\nvoid AppendIndex(uint argsOffset,uint index,RWStructuredBuffer<uint> indices){ uint oldVertices=0u; g_drawArgs.InterlockedAdd(argsOffset,6u,oldVertices); indices[oldVertices/6u]=index; }\n[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID){ uint i=id.x;\n if(i<g.capacity.x){ Particle p=g_particles[i]; if(p.generation==g.generation.x&&p.lifetimeSeconds>0.0&&p.ageSeconds<p.lifetimeSeconds) AppendIndex(0u,i,g_particleIndices); }\n if(i<g.capacity.y){ SplashState s=g_splashes[i]; if(s.generation==g.generation.y&&s.lifetimeSeconds>0.0&&s.ageSeconds<s.lifetimeSeconds) AppendIndex(16u,i,g_splashIndices); }\n if(i<g.capacity.z){ Droplet d=g_droplets[i]; if(d.generation==g.generation.z&&d.lifetimeSeconds>0.0&&d.ageSeconds<d.lifetimeSeconds) AppendIndex(32u,i,g_dropletIndices); }\n}\n)";\n\n'''
rep(p,'[[nodiscard]] u32 Bits(const f32 value) noexcept\n{',shader+'[[nodiscard]] u32 Bits(const f32 value) noexcept\n{')

# Allocate active lists + indirect args.
rep(p,
'''    { std::byte* mapped=zeroDropletCounterUpload_->Map(); std::memset(mapped,0,sizeof(u32)); zeroDropletCounterUpload_->Unmap(); }\n\n    spawnBuffers_.reserve(framesInFlight);''',
'''    { std::byte* mapped=zeroDropletCounterUpload_->Map(); std::memset(mapped,0,sizeof(u32)); zeroDropletCounterUpload_->Unmap(); }\n\n    particleActiveIndices_=device.CreateBuffer({.sizeBytes=static_cast<u64>(MaximumParticleCount)*sizeof(u32),.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::UnorderedAccess});\n    splashActiveIndices_=device.CreateBuffer({.sizeBytes=static_cast<u64>(MaximumPersistentSplashCount)*sizeof(u32),.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::UnorderedAccess});\n    dropletActiveIndices_=device.CreateBuffer({.sizeBytes=static_cast<u64>(MaximumDropletCount)*sizeof(u32),.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::UnorderedAccess});\n    indirectDrawArguments_=device.CreateBuffer({.sizeBytes=48U,.usage=rhi::BufferUsage::Indirect,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::CopyDestination});\n    zeroIndirectDrawArgumentsUpload_=device.CreateBuffer({.sizeBytes=48U,.usage=rhi::BufferUsage::Indirect,.memory=rhi::MemoryUsage::HostVisible,.initialState=rhi::ResourceState::CopySource});\n    { auto* mapped=reinterpret_cast<u32*>(zeroIndirectDrawArgumentsUpload_->Map()); std::memset(mapped,0,48U); mapped[1]=1U; mapped[5]=1U; mapped[9]=1U; zeroIndirectDrawArgumentsUpload_->Unmap(); }\n\n    spawnBuffers_.reserve(framesInFlight);''')

# Compile draw list pipeline.
rep(p,
'''    dropletCollisionPipeline_=device.CreateComputePipeline({.computeShader={.data=dropletCollision.bytecode.data(),.size=dropletCollision.bytecode.size()},.pushConstantDwords=12U,.shaderResourceBuffers=4U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});\n}''',
'''    dropletCollisionPipeline_=device.CreateComputePipeline({.computeShader={.data=dropletCollision.bytecode.data(),.size=dropletCollision.bytecode.size()},.pushConstantDwords=12U,.shaderResourceBuffers=4U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});\n    const auto drawList=compiler.Compile({.source=kDrawListShader,.entryPoint="main",.stage=shader::Stage::Compute,.debug=false});\n    if(drawList.bytecode.empty()) throw std::runtime_error("Orbit failed to compile the M38 GPU draw-list shader.");\n    drawListPipeline_=device.CreateComputePipeline({.computeShader={.data=drawList.bytecode.data(),.size=drawList.bytecode.size()},.pushConstantDwords=8U,.shaderResourceBuffers=7U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});\n}''')

# At end of collision pass, generate active lists and draw args.
rep(p,
'''    splashOriginDeltaMeters_ = {};\n    TransitionState(commands,splashDestination,splashDestinationState,rhi::ResourceState::ShaderResource);\n}\n''',
'''    splashOriginDeltaMeters_ = {};\n    TransitionState(commands,splashDestination,splashDestinationState,rhi::ResourceState::ShaderResource);\n\n    // Build compact GPU draw lists after all in-place collision kills. This\n    // avoids assuming the surviving states are still dense.\n    TransitionState(commands,*indirectDrawArguments_,indirectDrawArgumentsState_,rhi::ResourceState::CopyDestination);\n    commands.CopyBuffer(*zeroIndirectDrawArgumentsUpload_,0U,*indirectDrawArguments_,0U,48U);\n    TransitionState(commands,*indirectDrawArguments_,indirectDrawArgumentsState_,rhi::ResourceState::UnorderedAccess);\n    TransitionState(commands,*particleActiveIndices_,particleActiveIndicesState_,rhi::ResourceState::UnorderedAccess);\n    TransitionState(commands,*splashActiveIndices_,splashActiveIndicesState_,rhi::ResourceState::UnorderedAccess);\n    TransitionState(commands,*dropletActiveIndices_,dropletActiveIndicesState_,rhi::ResourceState::UnorderedAccess);\n    std::array<u32,8U> drawConstants{}; drawConstants[0]=generation_; drawConstants[1]=splashGeneration_; drawConstants[2]=dropletGeneration_; drawConstants[4]=MaximumParticleCount; drawConstants[5]=MaximumPersistentSplashCount; drawConstants[6]=MaximumDropletCount;\n    commands.SetComputePipeline(*drawListPipeline_); commands.SetComputeConstants(drawConstants);\n    commands.SetComputeBuffer(0U,current); commands.SetComputeBuffer(1U,splashDestination); commands.SetComputeBuffer(2U,dropletDestination); commands.SetComputeBuffer(3U,*particleActiveIndices_); commands.SetComputeBuffer(4U,*splashActiveIndices_); commands.SetComputeBuffer(5U,*dropletActiveIndices_); commands.SetComputeBuffer(6U,*indirectDrawArguments_);\n    commands.Dispatch((MaximumParticleCount+63U)/64U,1U,1U);\n    commands.UavBarrier(*particleActiveIndices_); commands.UavBarrier(*splashActiveIndices_); commands.UavBarrier(*dropletActiveIndices_); commands.UavBarrier(*indirectDrawArguments_);\n    TransitionState(commands,*particleActiveIndices_,particleActiveIndicesState_,rhi::ResourceState::ShaderResource);\n    TransitionState(commands,*splashActiveIndices_,splashActiveIndicesState_,rhi::ResourceState::ShaderResource);\n    TransitionState(commands,*dropletActiveIndices_,dropletActiveIndicesState_,rhi::ResourceState::ShaderResource);\n    TransitionState(commands,*indirectDrawArguments_,indirectDrawArgumentsState_,rhi::ResourceState::IndirectArgument);\n}\n''')

# Bind index lists, getter.
rep(p,
'''    commands.SetGraphicsBuffer(DropletGraphicsBufferSlot,dropletCurrent);\n}\n''',
'''    commands.SetGraphicsBuffer(DropletGraphicsBufferSlot,dropletCurrent);\n    TransitionState(commands,*particleActiveIndices_,particleActiveIndicesState_,rhi::ResourceState::ShaderResource);\n    TransitionState(commands,*splashActiveIndices_,splashActiveIndicesState_,rhi::ResourceState::ShaderResource);\n    TransitionState(commands,*dropletActiveIndices_,dropletActiveIndicesState_,rhi::ResourceState::ShaderResource);\n    commands.SetGraphicsBuffer(ParticleActiveIndexGraphicsBufferSlot,*particleActiveIndices_);\n    commands.SetGraphicsBuffer(SplashActiveIndexGraphicsBufferSlot,*splashActiveIndices_);\n    commands.SetGraphicsBuffer(DropletActiveIndexGraphicsBufferSlot,*dropletActiveIndices_);\n}\n''')
rep(p,
'''rhi::Buffer& VolumeParticleGpuState::CurrentBuffer() noexcept\n{\n    return currentIsA_ ? *stateA_ : *stateB_;\n}\n''',
'''rhi::Buffer& VolumeParticleGpuState::CurrentBuffer() noexcept\n{\n    return currentIsA_ ? *stateA_ : *stateB_;\n}\n\nrhi::Buffer& VolumeParticleGpuState::IndirectDrawArguments() noexcept\n{\n    return *indirectDrawArguments_;\n}\n''')

# Renderer: index indirection and indirect draws.
p='engine/volume_render/src/VolumeParticleRenderer.cpp'
rep(p,
'''[[vk::binding(2, 0)]]\nStructuredBuffer<Particle> g_particles : register(t2);\n''',
'''[[vk::binding(2, 0)]]\nStructuredBuffer<Particle> g_particles : register(t2);\n[[vk::binding(5, 0)]] StructuredBuffer<uint> g_particleIndices : register(t5);\n''')
rep(p,'    const uint particleIndex = vertexId / 6u;','    const uint particleIndex = g_particleIndices[vertexId / 6u];')
rep(p,
'''[[vk::binding(3, 0)]] StructuredBuffer<SplashState> g_splashes : register(t3);''',
'''[[vk::binding(3, 0)]] StructuredBuffer<SplashState> g_splashes : register(t3);\n[[vk::binding(6, 0)]] StructuredBuffer<uint> g_splashIndices : register(t6);''')
rep(p,'    uint eventIndex=vertexId/6u, cornerIndex=vertexId%6u; SplashState e=g_splashes[eventIndex]; VSOutput o;','    uint eventIndex=g_splashIndices[vertexId/6u], cornerIndex=vertexId%6u; SplashState e=g_splashes[eventIndex]; VSOutput o;')
rep(p,
'''[[vk::binding(4, 0)]] StructuredBuffer<Droplet> g_droplets : register(t4);''',
'''[[vk::binding(4, 0)]] StructuredBuffer<Droplet> g_droplets : register(t4);\n[[vk::binding(7, 0)]] StructuredBuffer<uint> g_dropletIndices : register(t7);''')
rep(p,'uint i=vertexId/6u,c=vertexId%6u; Droplet d=g_droplets[i]; O o;','uint i=g_dropletIndices[vertexId/6u],c=vertexId%6u; Droplet d=g_droplets[i]; O o;')
# all three graphics pipelines need bindings through t7
for old in ['.shaderResourceBuffers = 3U,','.shaderResourceBuffers=4U,','.shaderResourceBuffers=5U,']:
    if old in Path(p).read_text(encoding='utf-8'):
        rep(p,old,old.split('=')[0]+'= 8U,' if ' = ' in old else '.shaderResourceBuffers=8U,')
rep(p,
'''    commands.Draw(VolumeParticleGpuState::MaximumParticleCount * 6U);\n    commands.SetGraphicsPipeline(*splashPipeline_);''',
'''    commands.DrawIndirect(state_.IndirectDrawArguments(), VolumeParticleGpuState::ParticleIndirectOffsetBytes);\n    commands.SetGraphicsPipeline(*splashPipeline_);''')
rep(p,
'''    commands.Draw(VolumeParticleGpuState::MaximumPersistentSplashCount * 6U);\n    commands.SetGraphicsPipeline(*dropletPipeline_);''',
'''    commands.DrawIndirect(state_.IndirectDrawArguments(), VolumeParticleGpuState::SplashIndirectOffsetBytes);\n    commands.SetGraphicsPipeline(*dropletPipeline_);''')
rep(p,'    commands.Draw(VolumeParticleGpuState::MaximumDropletCount * 6U);','    commands.DrawIndirect(state_.IndirectDrawArguments(), VolumeParticleGpuState::DropletIndirectOffsetBytes);')

# Regression compile-time/API contract assertions.
p='engine/studio_ui/tests/VolumeParticleRenderBridgeTests.cpp'
rep(p,
'''    Check(volume_render::VolumeParticleGpuState::DropletGraphicsBufferSlot == 4U);\n''',
'''    Check(volume_render::VolumeParticleGpuState::DropletGraphicsBufferSlot == 4U);\n    Check(volume_render::VolumeParticleGpuState::ParticleActiveIndexGraphicsBufferSlot == 5U);\n    Check(volume_render::VolumeParticleGpuState::SplashActiveIndexGraphicsBufferSlot == 6U);\n    Check(volume_render::VolumeParticleGpuState::DropletActiveIndexGraphicsBufferSlot == 7U);\n    Check(volume_render::VolumeParticleGpuState::ParticleIndirectOffsetBytes == 0U);\n    Check(volume_render::VolumeParticleGpuState::SplashIndirectOffsetBytes == 16U);\n    Check(volume_render::VolumeParticleGpuState::DropletIndirectOffsetBytes == 32U);\n''')

print('GPU indirect draw patch applied')
