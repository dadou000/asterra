from pathlib import Path


def rep(path, old, new, count=1):
    p = Path(path)
    text = p.read_text(encoding='utf-8')
    n = text.count(old)
    if n != count:
        raise RuntimeError(f'{path}: expected {count} match(es), got {n}: {old[:180]!r}')
    p.write_text(text.replace(old, new, count), encoding='utf-8')

# Public GPU-state visibility build entry point.
p='engine/volume_render/include/orbit/volume_render/VolumeParticleGpuState.hpp'
rep(p,
'''    void BindForGraphics(rhi::CommandList& commands);\n    void Reset() noexcept;\n''',
'''    // Rebuilds compact active-index lists and indirect draw arguments for a\n    // specific viewport. This is deliberately presentation-time work: the\n    // simulation state remains shared/GPU-resident while each viewport gets\n    // its own frustum-filtered draw population immediately before rendering.\n    void BuildVisibleDrawLists(\n        rhi::CommandList& commands,\n        math::Float3 cameraPositionMeters,\n        math::Float3 cameraForward,\n        math::Float3 cameraUp,\n        f32 aspectRatio,\n        f32 tanHalfVerticalFov,\n        f32 nearPlaneMeters,\n        f32 farPlaneMeters);\n\n    void BindForGraphics(rhi::CommandList& commands);\n    void Reset() noexcept;\n''')

# Replace the draw-list shader with viewport-aware sphere-frustum culling.
p='engine/volume_render/src/VolumeParticleGpuState.cpp'
text=Path(p).read_text(encoding='utf-8')
start=text.index('constexpr const char* kDrawListShader = R"(')
end=text.index('\n)";\n\n[[nodiscard]] u32 Bits', start)+5
old=text[start:end]
new=r'''constexpr const char* kDrawListShader = R"(
struct Particle { float3 positionMeters; float authority; float3 velocityMetersPerSecond; float density; float emission; float ageSeconds; float lifetimeSeconds; float linearDragPerSecond; float radiusMeters; float emissionScale; float gravityScale; float restitution; float3 baseColor; uint behaviorFlags; float3 emissionColor; uint generation; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; float waterDensityRatio; float waterDragPerSecond; float waterBuoyancyScale; float waterSplashScale; uint4 bodyIdentity; };
struct SplashState { float3 positionMeters; float baseScaleMeters; float3 normal; float expansionMetersPerSecond; float3 tint; float impactSpeedMetersPerSecond; float ageSeconds; float lifetimeSeconds; uint generation; uint reserved; };
struct Droplet { float3 positionMeters; float radiusMeters; float3 velocityMetersPerSecond; float ageSeconds; float3 tint; float lifetimeSeconds; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; uint4 bodyIdentity; uint generation; uint flags; uint sourceParticleGeneration; uint reserved1; };
[[vk::binding(0,0)]] StructuredBuffer<Particle> g_particles : register(t0);
[[vk::binding(1,0)]] StructuredBuffer<SplashState> g_splashes : register(t1);
[[vk::binding(2,0)]] StructuredBuffer<Droplet> g_droplets : register(t2);
[[vk::binding(3,0)]] RWStructuredBuffer<uint> g_particleIndices : register(u3);
[[vk::binding(4,0)]] RWStructuredBuffer<uint> g_splashIndices : register(u4);
[[vk::binding(5,0)]] RWStructuredBuffer<uint> g_dropletIndices : register(u5);
[[vk::binding(6,0)]] RWByteAddressBuffer g_drawArgs : register(u6);
struct Push
{
    uint4 generation;
    uint4 capacity;
    float4 camera;  // xyz position, w tan(verticalFov/2)
    float4 forward; // xyz forward,  w aspect
    float4 up;      // xyz requested up, w near plane
    float4 clip;    // x far plane
};
[[vk::push_constant]] Push g;

bool VisibleSphere(float3 positionMeters, float radiusMeters)
{
    float3 forward = normalize(g.forward.xyz);
    float3 requestedUp = normalize(g.up.xyz);
    float3 right = normalize(cross(forward, requestedUp));
    float3 cameraUp = normalize(cross(right, forward));
    float3 relative = positionMeters - g.camera.xyz;
    float radius = max(radiusMeters, 0.0);
    float z = dot(relative, forward);
    float nearPlane = max(g.up.w, 0.0);
    float farPlane = max(g.clip.x, nearPlane + 0.001);
    if (z + radius < nearPlane || z - radius > farPlane) return false;

    // Sphere-vs-frustum-plane test expressed in camera space. Using z clamped
    // to the near plane keeps grazing spheres stable while the centre crosses
    // the near plane and intentionally errs on the conservative side.
    float projectedZ = max(z, nearPlane);
    float verticalLimit = projectedZ * max(g.camera.w, 0.0001) + radius;
    float horizontalLimit = projectedZ * max(g.camera.w, 0.0001) * max(g.forward.w, 0.0001) + radius;
    if (abs(dot(relative, cameraUp)) > verticalLimit) return false;
    if (abs(dot(relative, right)) > horizontalLimit) return false;
    return true;
}

void AppendParticleIndex(uint index){ uint oldVertices=0u; g_drawArgs.InterlockedAdd(0u,6u,oldVertices); g_particleIndices[oldVertices/6u]=index; }
void AppendSplashIndex(uint index){ uint oldVertices=0u; g_drawArgs.InterlockedAdd(16u,6u,oldVertices); g_splashIndices[oldVertices/6u]=index; }
void AppendDropletIndex(uint index){ uint oldVertices=0u; g_drawArgs.InterlockedAdd(32u,6u,oldVertices); g_dropletIndices[oldVertices/6u]=index; }
[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID)
{
    uint i=id.x;
    if(i<g.capacity.x)
    {
        Particle p=g_particles[i];
        if(p.generation==g.generation.x && p.lifetimeSeconds>0.0 && p.ageSeconds<p.lifetimeSeconds &&
           VisibleSphere(p.positionMeters,max(p.radiusMeters,0.001)))
            AppendParticleIndex(i);
    }
    if(i<g.capacity.y)
    {
        SplashState s=g_splashes[i];
        float splashRadius=max(s.baseScaleMeters+s.expansionMetersPerSecond*s.ageSeconds,0.01);
        if(s.generation==g.generation.y && s.lifetimeSeconds>0.0 && s.ageSeconds<s.lifetimeSeconds &&
           VisibleSphere(s.positionMeters,splashRadius))
            AppendSplashIndex(i);
    }
    if(i<g.capacity.z)
    {
        Droplet d=g_droplets[i];
        if(d.generation==g.generation.z && d.lifetimeSeconds>0.0 && d.ageSeconds<d.lifetimeSeconds &&
           VisibleSphere(d.positionMeters,max(d.radiusMeters,0.002)))
            AppendDropletIndex(i);
    }
}
)";'''
Path(p).write_text(text[:start]+new+text[end:], encoding='utf-8')

# Expand draw-list push constants.
rep(p,
'''    drawListPipeline_=device.CreateComputePipeline({.computeShader={.data=drawList.bytecode.data(),.size=drawList.bytecode.size()},.pushConstantDwords=8U,.shaderResourceBuffers=7U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});''',
'''    drawListPipeline_=device.CreateComputePipeline({.computeShader={.data=drawList.bytecode.data(),.size=drawList.bytecode.size()},.pushConstantDwords=24U,.shaderResourceBuffers=7U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});''')

# Remove global post-collision draw-list construction.
old_block='''    // Build compact GPU draw lists after all in-place collision kills. This\n    // avoids assuming the surviving states are still dense.\n    TransitionState(commands,*indirectDrawArguments_,indirectDrawArgumentsState_,rhi::ResourceState::CopyDestination);\n    commands.CopyBuffer(*zeroIndirectDrawArgumentsUpload_,0U,*indirectDrawArguments_,0U,48U);\n    TransitionState(commands,*indirectDrawArguments_,indirectDrawArgumentsState_,rhi::ResourceState::UnorderedAccess);\n    TransitionState(commands,*particleActiveIndices_,particleActiveIndicesState_,rhi::ResourceState::UnorderedAccess);\n    TransitionState(commands,*splashActiveIndices_,splashActiveIndicesState_,rhi::ResourceState::UnorderedAccess);\n    TransitionState(commands,*dropletActiveIndices_,dropletActiveIndicesState_,rhi::ResourceState::UnorderedAccess);\n    std::array<u32,8U> drawConstants{}; drawConstants[0]=generation_; drawConstants[1]=splashGeneration_; drawConstants[2]=dropletGeneration_; drawConstants[4]=MaximumParticleCount; drawConstants[5]=MaximumPersistentSplashCount; drawConstants[6]=MaximumDropletCount;\n    commands.SetComputePipeline(*drawListPipeline_); commands.SetComputeConstants(drawConstants);\n    commands.SetComputeBuffer(0U,current); commands.SetComputeBuffer(1U,splashDestination); commands.SetComputeBuffer(2U,dropletDestination); commands.SetComputeBuffer(3U,*particleActiveIndices_); commands.SetComputeBuffer(4U,*splashActiveIndices_); commands.SetComputeBuffer(5U,*dropletActiveIndices_); commands.SetComputeBuffer(6U,*indirectDrawArguments_);\n    commands.Dispatch((MaximumParticleCount+63U)/64U,1U,1U);\n    commands.UavBarrier(*particleActiveIndices_); commands.UavBarrier(*splashActiveIndices_); commands.UavBarrier(*dropletActiveIndices_); commands.UavBarrier(*indirectDrawArguments_);\n    TransitionState(commands,*particleActiveIndices_,particleActiveIndicesState_,rhi::ResourceState::ShaderResource);\n    TransitionState(commands,*splashActiveIndices_,splashActiveIndicesState_,rhi::ResourceState::ShaderResource);\n    TransitionState(commands,*dropletActiveIndices_,dropletActiveIndicesState_,rhi::ResourceState::ShaderResource);\n    TransitionState(commands,*indirectDrawArguments_,indirectDrawArgumentsState_,rhi::ResourceState::IndirectArgument);\n'''
rep(p, old_block, '')

# Add per-viewport list builder before BindForGraphics.
marker='''void VolumeParticleGpuState::BindForGraphics(\n    rhi::CommandList& commands)\n{'''
method=r'''void VolumeParticleGpuState::BuildVisibleDrawLists(
    rhi::CommandList& commands,
    const math::Float3 cameraPositionMeters,
    const math::Float3 cameraForward,
    const math::Float3 cameraUp,
    const f32 aspectRatio,
    const f32 tanHalfVerticalFov,
    const f32 nearPlaneMeters,
    const f32 farPlaneMeters)
{
    if (!initialized_ || generation_ == 0U)
    {
        return;
    }

    rhi::Buffer& particleCurrent = CurrentBuffer();
    rhi::ResourceState& particleState = currentIsA_ ? stateAState_ : stateBState_;
    rhi::Buffer& splashCurrent = splashCurrentIsA_ ? *splashStateA_ : *splashStateB_;
    rhi::ResourceState& splashState = splashCurrentIsA_ ? splashStateAState_ : splashStateBState_;
    rhi::Buffer& dropletCurrent = dropletCurrentIsA_ ? *dropletStateA_ : *dropletStateB_;
    rhi::ResourceState& dropletState = dropletCurrentIsA_ ? dropletStateAState_ : dropletStateBState_;

    // Compute buffers use storage descriptors in the current RHI, so expose
    // the three state pools as UAV-readable for the cull and return them to
    // graphics SRV state before rasterization.
    TransitionState(commands, particleCurrent, particleState, rhi::ResourceState::UnorderedAccess);
    TransitionState(commands, splashCurrent, splashState, rhi::ResourceState::UnorderedAccess);
    TransitionState(commands, dropletCurrent, dropletState, rhi::ResourceState::UnorderedAccess);

    TransitionState(commands,*indirectDrawArguments_,indirectDrawArgumentsState_,rhi::ResourceState::CopyDestination);
    commands.CopyBuffer(*zeroIndirectDrawArgumentsUpload_,0U,*indirectDrawArguments_,0U,48U);
    TransitionState(commands,*indirectDrawArguments_,indirectDrawArgumentsState_,rhi::ResourceState::UnorderedAccess);
    TransitionState(commands,*particleActiveIndices_,particleActiveIndicesState_,rhi::ResourceState::UnorderedAccess);
    TransitionState(commands,*splashActiveIndices_,splashActiveIndicesState_,rhi::ResourceState::UnorderedAccess);
    TransitionState(commands,*dropletActiveIndices_,dropletActiveIndicesState_,rhi::ResourceState::UnorderedAccess);

    const f32 safeAspect = std::isfinite(aspectRatio) ? std::max(aspectRatio, 0.0001F) : 1.0F;
    const f32 safeTanHalfFov = std::isfinite(tanHalfVerticalFov) ? std::max(tanHalfVerticalFov, 0.0001F) : 1.0F;
    const f32 safeNear = std::isfinite(nearPlaneMeters) ? std::max(nearPlaneMeters, 0.0F) : 0.0F;
    const f32 safeFar = std::isfinite(farPlaneMeters) ? std::max(farPlaneMeters, safeNear + 0.001F) : safeNear + 1.0F;

    std::array<u32,24U> c{};
    c[0]=generation_; c[1]=splashGeneration_; c[2]=dropletGeneration_;
    c[4]=MaximumParticleCount; c[5]=MaximumPersistentSplashCount; c[6]=MaximumDropletCount;
    c[8]=Bits(cameraPositionMeters.x); c[9]=Bits(cameraPositionMeters.y); c[10]=Bits(cameraPositionMeters.z); c[11]=Bits(safeTanHalfFov);
    c[12]=Bits(cameraForward.x); c[13]=Bits(cameraForward.y); c[14]=Bits(cameraForward.z); c[15]=Bits(safeAspect);
    c[16]=Bits(cameraUp.x); c[17]=Bits(cameraUp.y); c[18]=Bits(cameraUp.z); c[19]=Bits(safeNear);
    c[20]=Bits(safeFar);

    commands.SetComputePipeline(*drawListPipeline_);
    commands.SetComputeConstants(c);
    commands.SetComputeBuffer(0U,particleCurrent);
    commands.SetComputeBuffer(1U,splashCurrent);
    commands.SetComputeBuffer(2U,dropletCurrent);
    commands.SetComputeBuffer(3U,*particleActiveIndices_);
    commands.SetComputeBuffer(4U,*splashActiveIndices_);
    commands.SetComputeBuffer(5U,*dropletActiveIndices_);
    commands.SetComputeBuffer(6U,*indirectDrawArguments_);
    commands.Dispatch((MaximumParticleCount+63U)/64U,1U,1U);
    commands.UavBarrier(*particleActiveIndices_);
    commands.UavBarrier(*splashActiveIndices_);
    commands.UavBarrier(*dropletActiveIndices_);
    commands.UavBarrier(*indirectDrawArguments_);

    TransitionState(commands,particleCurrent,particleState,rhi::ResourceState::ShaderResource);
    TransitionState(commands,splashCurrent,splashState,rhi::ResourceState::ShaderResource);
    TransitionState(commands,dropletCurrent,dropletState,rhi::ResourceState::ShaderResource);
    TransitionState(commands,*particleActiveIndices_,particleActiveIndicesState_,rhi::ResourceState::ShaderResource);
    TransitionState(commands,*splashActiveIndices_,splashActiveIndicesState_,rhi::ResourceState::ShaderResource);
    TransitionState(commands,*dropletActiveIndices_,dropletActiveIndicesState_,rhi::ResourceState::ShaderResource);
    TransitionState(commands,*indirectDrawArguments_,indirectDrawArgumentsState_,rhi::ResourceState::IndirectArgument);
}

'''
rep(p, marker, method+marker)

# Renderer: build this viewport's compact lists before entering dynamic rendering.
p='engine/volume_render/src/VolumeParticleRenderer.cpp'
old='''    constants[19] = state_.Generation();\n\n    commands.SetRenderTargets(sceneColor, depth);'''
new='''    constants[19] = state_.Generation();\n\n    state_.BuildVisibleDrawLists(\n        commands,\n        {\n            static_cast<f32>(cameraPositionRelativeToPresentationOriginMeters.x),\n            static_cast<f32>(cameraPositionRelativeToPresentationOriginMeters.y),\n            static_cast<f32>(cameraPositionRelativeToPresentationOriginMeters.z)\n        },\n        camera.forward,\n        camera.up,\n        static_cast<f32>(width) / static_cast<f32>(height),\n        tanHalfFov,\n        camera.nearPlaneMeters,\n        camera.farPlaneMeters);\n\n    commands.SetRenderTargets(sceneColor, depth);'''
rep(p, old, new)

print('M38 viewport visibility culling patch applied')
