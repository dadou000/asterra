from pathlib import Path

def rep(path, old, new):
    p=Path(path); t=p.read_text(encoding='utf-8'); c=t.count(old)
    if c!=1: raise RuntimeError(f'{path}: expected 1 match got {c}: {old[:120]!r}')
    p.write_text(t.replace(old,new,1),encoding='utf-8')

# -----------------------------------------------------------------------------
# Header: enrich splash event with body authority and add persistent droplets.
# -----------------------------------------------------------------------------
p='engine/volume_render/include/orbit/volume_render/VolumeParticleGpuState.hpp'
rep(p,
'''struct VolumeParticleGpuSplashEvent
{
    math::Float3 positionMeters{};
    f32 scaleMeters{0.0F};
    math::Float3 normal{};
    f32 impactSpeedMetersPerSecond{0.0F};
    math::Float3 tint{1.0F, 1.0F, 1.0F};
    u32 generation{0U};
};
static_assert(sizeof(VolumeParticleGpuSplashEvent) == 48U);
''',
'''struct VolumeParticleGpuSplashEvent
{
    math::Float3 positionMeters{};
    f32 scaleMeters{0.0F};
    math::Float3 normal{};
    f32 impactSpeedMetersPerSecond{0.0F};
    math::Float3 tint{1.0F, 1.0F, 1.0F};
    u32 generation{0U};
    math::Float3 bodyCenterMeters{};
    f32 gravitationalParameterM3PerS2{0.0F};
    math::Float3 surfaceRadiiMeters{};
    f32 gravitySofteningMeters{0.0F};
    std::array<u32, 4U> bodyIdentity{};
    u32 flags{0U};
    std::array<u32, 3U> reserved{};
};
static_assert(sizeof(VolumeParticleGpuSplashEvent) == 112U);
''')
rep(p,
'''static_assert(sizeof(VolumeParticleGpuSplashState) == 64U);

struct VolumeParticleSimulationSettings''',
'''static_assert(sizeof(VolumeParticleGpuSplashState) == 64U);

struct VolumeParticleGpuDropletState
{
    math::Float3 positionMeters{};
    f32 radiusMeters{0.0F};
    math::Float3 velocityMetersPerSecond{};
    f32 ageSeconds{0.0F};
    math::Float3 tint{1.0F, 1.0F, 1.0F};
    f32 lifetimeSeconds{0.0F};
    math::Float3 bodyCenterMeters{};
    f32 gravitationalParameterM3PerS2{0.0F};
    math::Float3 surfaceRadiiMeters{};
    f32 gravitySofteningMeters{0.0F};
    std::array<u32, 4U> bodyIdentity{};
    u32 generation{0U};
    u32 flags{0U};
    std::array<u32, 2U> reserved{};
};
static_assert(sizeof(VolumeParticleGpuDropletState) == 112U);

struct VolumeParticleSimulationSettings''')
rep(p,
'''    static constexpr u32 MaximumPersistentSplashCount = 8192U;
    static constexpr u32 ComputeBufferCount = 4U;
    static constexpr u32 GraphicsBufferSlot = 2U;
    static constexpr u32 SplashGraphicsBufferSlot = 3U;
''',
'''    static constexpr u32 MaximumPersistentSplashCount = 8192U;
    static constexpr u32 MaximumDropletCount = 16384U;
    static constexpr u32 MaximumDropletsPerSplash = 8U;
    static constexpr u32 ComputeBufferCount = 4U;
    static constexpr u32 GraphicsBufferSlot = 2U;
    static constexpr u32 SplashGraphicsBufferSlot = 3U;
    static constexpr u32 DropletGraphicsBufferSlot = 4U;
''')
rep(p,
'''    [[nodiscard]] u32 Generation() const noexcept;
    [[nodiscard]] u32 SubmittedSpawnCount() const noexcept;
    [[nodiscard]] rhi::Buffer& CurrentBuffer() noexcept;
''',
'''    [[nodiscard]] u32 Generation() const noexcept;
    [[nodiscard]] u32 SplashGeneration() const noexcept;
    [[nodiscard]] u32 DropletGeneration() const noexcept;
    [[nodiscard]] u32 SubmittedSpawnCount() const noexcept;
    [[nodiscard]] rhi::Buffer& CurrentBuffer() noexcept;
''')
rep(p,
'''    u32 splashGeneration_{0U};
    bool initialized_{false};
    bool currentIsA_{true};
    bool splashCurrentIsA_{true};
    math::Double3 splashOriginDeltaMeters_{};
''',
'''    u32 splashGeneration_{0U};
    u32 dropletGeneration_{0U};
    bool initialized_{false};
    bool currentIsA_{true};
    bool splashCurrentIsA_{true};
    bool dropletCurrentIsA_{true};
    math::Double3 splashOriginDeltaMeters_{};
''')
rep(p,
'''    rhi::ResourceState splashStateCounterState_{rhi::ResourceState::CopyDestination};
''',
'''    rhi::ResourceState splashStateCounterState_{rhi::ResourceState::CopyDestination};
    rhi::ResourceState dropletStateAState_{rhi::ResourceState::CopyDestination};
    rhi::ResourceState dropletStateBState_{rhi::ResourceState::CopyDestination};
    rhi::ResourceState dropletCounterState_{rhi::ResourceState::CopyDestination};
''')
rep(p,
'''    std::unique_ptr<rhi::Buffer> splashStateCounter_;
    std::unique_ptr<rhi::Buffer> zeroSplashStateCounterUpload_;
''',
'''    std::unique_ptr<rhi::Buffer> splashStateCounter_;
    std::unique_ptr<rhi::Buffer> zeroSplashStateCounterUpload_;
    std::unique_ptr<rhi::Buffer> dropletStateA_;
    std::unique_ptr<rhi::Buffer> dropletStateB_;
    std::unique_ptr<rhi::Buffer> zeroDropletStateUpload_;
    std::unique_ptr<rhi::Buffer> dropletCounter_;
    std::unique_ptr<rhi::Buffer> zeroDropletCounterUpload_;
''')
rep(p,
'''    std::unique_ptr<rhi::ComputePipeline> terrainCollisionPipeline_;
    std::unique_ptr<rhi::ComputePipeline> splashSimulationPipeline_;
''',
'''    std::unique_ptr<rhi::ComputePipeline> terrainCollisionPipeline_;
    std::unique_ptr<rhi::ComputePipeline> splashSimulationPipeline_;
    std::unique_ptr<rhi::ComputePipeline> dropletSimulationPipeline_;
    std::unique_ptr<rhi::ComputePipeline> dropletCollisionPipeline_;
''')

# -----------------------------------------------------------------------------
# GPU state source: richer splash event in particle terrain collision.
# -----------------------------------------------------------------------------
p='engine/volume_render/src/VolumeParticleGpuState.cpp'
rep(p,
'''struct SplashEvent
{
    float3 positionMeters;
    float scaleMeters;
    float3 normal;
    float impactSpeedMetersPerSecond;
    float3 tint;
    uint generation;
};
''',
'''struct SplashEvent
{
    float3 positionMeters;
    float scaleMeters;
    float3 normal;
    float impactSpeedMetersPerSecond;
    float3 tint;
    uint generation;
    float3 bodyCenterMeters;
    float gravitationalParameterM3PerS2;
    float3 surfaceRadiiMeters;
    float gravitySofteningMeters;
    uint4 bodyIdentity;
    uint flags;
    uint3 reserved;
};
''')
rep(p,
'''        event.tint = lerp(float3(1.0, 1.0, 1.0), max(particle.baseColor, 0.0), 0.15);
        event.generation = g.meta.y;
        g_splashes[eventIndex] = event;
''',
'''        event.tint = lerp(float3(1.0, 1.0, 1.0), max(particle.baseColor, 0.0), 0.15);
        event.generation = g.meta.y;
        event.bodyCenterMeters = particle.bodyCenterMeters;
        event.gravitationalParameterM3PerS2 = particle.gravitationalParameterM3PerS2;
        event.surfaceRadiiMeters = particle.surfaceRadiiMeters;
        event.gravitySofteningMeters = particle.gravitySofteningMeters;
        event.bodyIdentity = particle.bodyIdentity;
        event.flags = 1u; // Primary particle entry may emit secondary droplets.
        event.reserved = 0u;
        g_splashes[eventIndex] = event;
''')

# Persistent splash consumer only needs the prefix but declaration must match stride.
rep(p,
'''struct SplashEvent { float3 positionMeters; float scaleMeters; float3 normal; float impactSpeedMetersPerSecond; float3 tint; uint generation; };
struct SplashState''',
'''struct SplashEvent { float3 positionMeters; float scaleMeters; float3 normal; float impactSpeedMetersPerSecond; float3 tint; uint generation; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; uint4 bodyIdentity; uint flags; uint3 reserved; };
struct SplashState''')

# Add droplet simulation + terrain collision shaders before splash simulation.
marker='constexpr const char* kSplashSimulationShader = R"('
droplet_shaders=r'''constexpr const char* kDropletSimulationShader = R"(
struct SplashEvent { float3 positionMeters; float scaleMeters; float3 normal; float impactSpeedMetersPerSecond; float3 tint; uint generation; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; uint4 bodyIdentity; uint flags; uint3 reserved; };
struct Droplet { float3 positionMeters; float radiusMeters; float3 velocityMetersPerSecond; float ageSeconds; float3 tint; float lifetimeSeconds; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; uint4 bodyIdentity; uint generation; uint flags; uint2 reserved; };
[[vk::binding(0,0)]] RWStructuredBuffer<Droplet> g_source : register(u0);
[[vk::binding(1,0)]] RWStructuredBuffer<Droplet> g_destination : register(u1);
[[vk::binding(2,0)]] RWStructuredBuffer<SplashEvent> g_events : register(u2);
[[vk::binding(3,0)]] RWStructuredBuffer<uint> g_eventCounter : register(u3);
[[vk::binding(4,0)]] RWStructuredBuffer<uint> g_counter : register(u4);
struct Push { uint4 counts; float4 timing; };
[[vk::push_constant]] Push g;
void Append(Droplet d){ uint index=0u; InterlockedAdd(g_counter[0],1u,index); if(index<g.counts.w){ d.generation=g.counts.y; g_destination[index]=d; } }
uint Hash(uint x){ x^=x>>16; x*=0x7feb352du; x^=x>>15; x*=0x846ca68bu; x^=x>>16; return x; }
float Unit01(uint x){ return float(Hash(x)&0x00ffffffu)/16777215.0; }
void Basis(float3 n,out float3 t,out float3 b){ float3 a=abs(n.z)<0.9?float3(0,0,1):float3(0,1,0); t=normalize(cross(a,n)); b=normalize(cross(n,t)); }
[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID){
 uint i=id.x; float dt=max(g.timing.x,0.0); float3 originDelta=g.timing.yzw;
 if(i<g.counts.w){ Droplet d=g_source[i]; if(d.generation==g.counts.x&&d.lifetimeSeconds>0.0){ d.ageSeconds+=dt; if(d.ageSeconds<d.lifetimeSeconds){ d.positionMeters+=originDelta; d.bodyCenterMeters+=originDelta; float3 radial=d.positionMeters-d.bodyCenterMeters; float soft=max(d.gravitySofteningMeters,0.0); float r2=dot(radial,radial)+soft*soft; if(r2>1e-6&&d.gravitationalParameterM3PerS2>0.0){ float inv=rsqrt(r2); d.velocityMetersPerSecond+=-radial*d.gravitationalParameterM3PerS2*inv*inv*inv*dt; } d.positionMeters+=d.velocityMetersPerSecond*dt; Append(d); } } }
 uint eventCount=min(g_eventCounter[0],4096u); if(i<eventCount){ SplashEvent e=g_events[i]; if(e.generation==g.counts.z&&(e.flags&1u)!=0u&&e.gravitationalParameterM3PerS2>0.0){ float3 radial=e.positionMeters-e.bodyCenterMeters; float r2=max(dot(radial,radial),1e-6); float gravity=e.gravitationalParameterM3PerS2/r2; float characteristic=sqrt(max(2.0*gravity*max(e.scaleMeters,0.01),0.01)); float energyRatio=e.impactSpeedMetersPerSecond/max(characteristic,0.01); if(energyRatio>1.0){ uint count=min(8u,max(1u,uint(floor(energyRatio)))); float3 n=normalize(e.normal); float3 t,b; Basis(n,t,b); float excess=max(e.impactSpeedMetersPerSecond-characteristic,0.0); [loop] for(uint k=0u;k<count;++k){ float u=Unit01(i*17u+k*131u+g.counts.y*7u); float v=Unit01(i*43u+k*197u+g.counts.y*11u); float angle=6.28318530718*u; float radialMix=sqrt(v); float3 lateral=(cos(angle)*t+sin(angle)*b)*radialMix; float3 dir=normalize(n*(1.0-0.45*radialMix)+lateral*0.45); Droplet d; d.positionMeters=e.positionMeters+n*max(e.scaleMeters*0.04,0.002); d.radiusMeters=max(e.scaleMeters/max(float(count)*12.0,24.0),0.002); d.velocityMetersPerSecond=dir*max(excess,0.25*characteristic); d.ageSeconds=0.0; d.tint=max(e.tint,0.0); d.lifetimeSeconds=clamp(0.5+2.0*length(d.velocityMetersPerSecond)/max(gravity,0.1),0.5,4.0); d.bodyCenterMeters=e.bodyCenterMeters; d.gravitationalParameterM3PerS2=e.gravitationalParameterM3PerS2; d.surfaceRadiiMeters=e.surfaceRadiiMeters; d.gravitySofteningMeters=e.gravitySofteningMeters; d.bodyIdentity=e.bodyIdentity; d.generation=g.counts.y; d.flags=0u; d.reserved=0u; Append(d); } } } }
}
)";

constexpr const char* kDropletCollisionShader = R"(
struct Droplet { float3 positionMeters; float radiusMeters; float3 velocityMetersPerSecond; float ageSeconds; float3 tint; float lifetimeSeconds; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; uint4 bodyIdentity; uint generation; uint flags; uint2 reserved; };
struct SplashEvent { float3 positionMeters; float scaleMeters; float3 normal; float impactSpeedMetersPerSecond; float3 tint; uint generation; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; uint4 bodyIdentity; uint flags; uint3 reserved; };
[[vk::binding(0,0)]] RWStructuredBuffer<Droplet> g_droplets : register(u0);
[[vk::binding(1,0)]] ByteAddressBuffer g_physicalPage : register(t1);
[[vk::binding(2,0)]] RWStructuredBuffer<SplashEvent> g_splashes : register(u2);
[[vk::binding(3,0)]] RWStructuredBuffer<uint> g_splashCounter : register(u3);
struct Push { uint4 tile; uint4 meta; uint4 body; };
[[vk::push_constant]] Push g;
static const uint kPhysicalTexelStrideBytes=8u;
void DirectionToCube(float3 d,out uint face,out float2 uv){ float3 u=normalize(d); float ax=abs(u.x),ay=abs(u.y),az=abs(u.z); if(ax>=ay&&ax>=az){if(u.x>=0){face=0;uv=float2(-u.z/ax,u.y/ax);}else{face=1;uv=float2(u.z/ax,u.y/ax);}}else if(ay>=ax&&ay>=az){if(u.y>=0){face=2;uv=float2(u.x/ay,-u.z/ay);}else{face=3;uv=float2(u.x/ay,u.z/ay);}}else{if(u.z>=0){face=4;uv=float2(u.x/az,u.y/az);}else{face=5;uv=float2(-u.x/az,u.y/az);}} uv=clamp(uv,-1.0,1.0); }
uint CoordinateToTileIndex(float c,uint count){ float n=clamp(c*0.5+0.5,0.0,1.0); if(n>=1.0)return count-1u; return uint(n*float(count)); }
bool BelongsToPage(uint face,float2 uv){ if(face!=g.tile.x)return false; uint level=min(g.tile.y,30u),count=1u<<level; return CoordinateToTileIndex(uv.x,count)==g.tile.z&&CoordinateToTileIndex(uv.y,count)==g.tile.w; }
float2 PageBoundsMin(){ uint count=1u<<min(g.tile.y,30u); return float2(-1.0+2.0*float(g.tile.z)/float(count),-1.0+2.0*float(g.tile.w)/float(count)); }
float2 PageBoundsMax(){ uint count=1u<<min(g.tile.y,30u); return float2(-1.0+2.0*float(g.tile.z+1u)/float(count),-1.0+2.0*float(g.tile.w+1u)/float(count)); }
float2 LoadPhysicalTexel(uint x,uint y){ uint index=y*g.meta.x+x; return asfloat(g_physicalPage.Load2(index*kPhysicalTexelStrideBytes)); }
float2 SamplePhysicalPage(float2 uv){ float2 mn=PageBoundsMin(),mx=PageBoundsMax(),extent=max(mx-mn,1e-7); float2 c=saturate((uv-mn)/extent)*float(max(g.meta.x-1u,1u)); uint2 p0=uint2(floor(c)),p1=min(p0+1u,uint2(g.meta.x-1u,g.meta.x-1u)); float2 f=c-float2(p0); float2 a=lerp(LoadPhysicalTexel(p0.x,p0.y),LoadPhysicalTexel(p1.x,p0.y),f.x); float2 b=lerp(LoadPhysicalTexel(p0.x,p1.y),LoadPhysicalTexel(p1.x,p1.y),f.x); return lerp(a,b,f.y); }
float ReferenceRadius(float3 d,float3 r){ float inv=sqrt(dot(d/r,d/r)); return inv>0.0?1.0/inv:0.0; }
bool SameBody(uint4 a,uint4 b){ return all(a==b); }
void EmitChildSplash(Droplet d,float3 position,float3 normal,float speed){ uint index=0u; InterlockedAdd(g_splashCounter[0],1u,index); if(index<4096u){ SplashEvent e; e.positionMeters=position; e.scaleMeters=max(d.radiusMeters*6.0,0.01); e.normal=normal; e.impactSpeedMetersPerSecond=max(speed,0.0); e.tint=d.tint; e.generation=g.meta.y; e.bodyCenterMeters=d.bodyCenterMeters; e.gravitationalParameterM3PerS2=d.gravitationalParameterM3PerS2; e.surfaceRadiiMeters=d.surfaceRadiiMeters; e.gravitySofteningMeters=d.gravitySofteningMeters; e.bodyIdentity=d.bodyIdentity; e.flags=0u; e.reserved=0u; g_splashes[index]=e; } }
[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID){ uint i=id.x; if(i>=g.body.z)return; Droplet d=g_droplets[i]; if(d.generation!=g.meta.y||!SameBody(d.bodyIdentity,uint4(g.meta.z,g.meta.w,g.body.x,g.body.y)))return; float3 local=d.positionMeters-d.bodyCenterMeters; float dist=length(local); if(dist<=1e-6)return; float3 dir=local/dist; uint face; float2 uv; DirectionToCube(dir,face,uv); if(!BelongsToPage(face,uv))return; float2 physical=SamplePhysicalPage(uv); float reference=ReferenceRadius(dir,max(d.surfaceRadiiMeters,0.001)); if(reference<=0.0)return; float terrain=reference+physical.x; float water=terrain+max(physical.y,0.0); float previousDist=length((d.positionMeters-d.velocityMetersPerSecond*max(asfloat(g.body.w),0.0))-d.bodyCenterMeters); if(physical.y>0.0&&previousDist>water&&dist<=water+d.radiusMeters){ float3 n=dir; float speed=max(-dot(d.velocityMetersPerSecond,n),0.0); EmitChildSplash(d,d.bodyCenterMeters+dir*water,n,speed); d.generation=0u; g_droplets[i]=d; return; } if(dist<=terrain+d.radiusMeters){ d.generation=0u; g_droplets[i]=d; } }
)";

'''
rep(p, marker, droplet_shaders+marker)

# -----------------------------------------------------------------------------
# Allocate droplet GPU buffers and compile pipelines.
# -----------------------------------------------------------------------------
rep(p,
'''    { std::byte* mapped=zeroSplashStateCounterUpload_->Map(); std::memset(mapped,0,sizeof(u32)); zeroSplashStateCounterUpload_->Unmap(); }

    spawnBuffers_.reserve(framesInFlight);''',
'''    { std::byte* mapped=zeroSplashStateCounterUpload_->Map(); std::memset(mapped,0,sizeof(u32)); zeroSplashStateCounterUpload_->Unmap(); }

    const u64 dropletBytes=static_cast<u64>(MaximumDropletCount)*sizeof(VolumeParticleGpuDropletState);
    const rhi::BufferDesc dropletDesc{.sizeBytes=dropletBytes,.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::CopyDestination};
    dropletStateA_=device.CreateBuffer(dropletDesc); dropletStateB_=device.CreateBuffer(dropletDesc);
    zeroDropletStateUpload_=device.CreateBuffer({.sizeBytes=dropletBytes,.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::HostVisible,.initialState=rhi::ResourceState::CopySource});
    { std::byte* mapped=zeroDropletStateUpload_->Map(); std::memset(mapped,0,static_cast<std::size_t>(dropletBytes)); zeroDropletStateUpload_->Unmap(); }
    dropletCounter_=device.CreateBuffer({.sizeBytes=sizeof(u32),.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::CopyDestination});
    zeroDropletCounterUpload_=device.CreateBuffer({.sizeBytes=sizeof(u32),.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::HostVisible,.initialState=rhi::ResourceState::CopySource});
    { std::byte* mapped=zeroDropletCounterUpload_->Map(); std::memset(mapped,0,sizeof(u32)); zeroDropletCounterUpload_->Unmap(); }

    spawnBuffers_.reserve(framesInFlight);''')
rep(p,
'''    const auto splashSimulation=compiler.Compile({.source=kSplashSimulationShader,.entryPoint="main",.stage=shader::Stage::Compute,.debug=false});
    if(splashSimulation.bytecode.empty()) throw std::runtime_error("Orbit failed to compile the M38 persistent splash simulation shader.");
    splashSimulationPipeline_=device.CreateComputePipeline({.computeShader={.data=splashSimulation.bytecode.data(),.size=splashSimulation.bytecode.size()},.pushConstantDwords=8U,.shaderResourceBuffers=5U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});
}''',
'''    const auto splashSimulation=compiler.Compile({.source=kSplashSimulationShader,.entryPoint="main",.stage=shader::Stage::Compute,.debug=false});
    if(splashSimulation.bytecode.empty()) throw std::runtime_error("Orbit failed to compile the M38 persistent splash simulation shader.");
    splashSimulationPipeline_=device.CreateComputePipeline({.computeShader={.data=splashSimulation.bytecode.data(),.size=splashSimulation.bytecode.size()},.pushConstantDwords=8U,.shaderResourceBuffers=5U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});
    const auto dropletSimulation=compiler.Compile({.source=kDropletSimulationShader,.entryPoint="main",.stage=shader::Stage::Compute,.debug=false});
    const auto dropletCollision=compiler.Compile({.source=kDropletCollisionShader,.entryPoint="main",.stage=shader::Stage::Compute,.debug=false});
    if(dropletSimulation.bytecode.empty()||dropletCollision.bytecode.empty()) throw std::runtime_error("Orbit failed to compile the M38 secondary droplet shaders.");
    dropletSimulationPipeline_=device.CreateComputePipeline({.computeShader={.data=dropletSimulation.bytecode.data(),.size=dropletSimulation.bytecode.size()},.pushConstantDwords=8U,.shaderResourceBuffers=5U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});
    dropletCollisionPipeline_=device.CreateComputePipeline({.computeShader={.data=dropletCollision.bytecode.data(),.size=dropletCollision.bytecode.size()},.pushConstantDwords=12U,.shaderResourceBuffers=4U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});
}''')

# Initialize/reset droplet state.
rep(p,
'''    commands.CopyBuffer(*zeroSplashStateCounterUpload_,0U,*splashStateCounter_,0U,sizeof(u32));

    TransitionState(''',
'''    commands.CopyBuffer(*zeroSplashStateCounterUpload_,0U,*splashStateCounter_,0U,sizeof(u32));
    TransitionState(commands,*dropletStateA_,dropletStateAState_,rhi::ResourceState::CopyDestination);
    TransitionState(commands,*dropletStateB_,dropletStateBState_,rhi::ResourceState::CopyDestination);
    TransitionState(commands,*dropletCounter_,dropletCounterState_,rhi::ResourceState::CopyDestination);
    commands.CopyBuffer(*zeroDropletStateUpload_,0U,*dropletStateA_,0U,dropletStateA_->SizeBytes());
    commands.CopyBuffer(*zeroDropletStateUpload_,0U,*dropletStateB_,0U,dropletStateB_->SizeBytes());
    commands.CopyBuffer(*zeroDropletCounterUpload_,0U,*dropletCounter_,0U,sizeof(u32));

    TransitionState(''')
rep(p,
'''    splashCurrentIsA_ = true;
    generation_ = 0U;
    splashGeneration_ = 0U;
    initialized_ = true;''',
'''    splashCurrentIsA_ = true;
    dropletCurrentIsA_ = true;
    generation_ = 0U;
    splashGeneration_ = 0U;
    dropletGeneration_ = 0U;
    initialized_ = true;''')

# -----------------------------------------------------------------------------
# ApplyTerrainCollision ordering: primary entries -> droplets -> droplet re-entry
# -> persistent foam. Insert before persistent splash compaction block.
# -----------------------------------------------------------------------------
needle='''    // Compact/age persistent splash state and append this generation's exact-once water-entry events.
    rhi::Buffer& splashSource=splashCurrentIsA_?*splashStateA_:*splashStateB_;'''
block=r'''    // Advance/compact ballistic droplets and spawn a bounded burst only from
    // primary splash events (event flags bit 0). Child re-entry splashes clear
    // that bit, preventing recursive spray explosions.
    rhi::Buffer& dropletSource=dropletCurrentIsA_?*dropletStateA_:*dropletStateB_;
    rhi::Buffer& dropletDestination=dropletCurrentIsA_?*dropletStateB_:*dropletStateA_;
    rhi::ResourceState& dropletSourceState=dropletCurrentIsA_?dropletStateAState_:dropletStateBState_;
    rhi::ResourceState& dropletDestinationState=dropletCurrentIsA_?dropletStateBState_:dropletStateAState_;
    TransitionState(commands,dropletSource,dropletSourceState,rhi::ResourceState::UnorderedAccess);
    TransitionState(commands,dropletDestination,dropletDestinationState,rhi::ResourceState::UnorderedAccess);
    TransitionState(commands,*dropletCounter_,dropletCounterState_,rhi::ResourceState::CopyDestination);
    commands.CopyBuffer(*zeroDropletCounterUpload_,0U,*dropletCounter_,0U,sizeof(u32));
    TransitionState(commands,*dropletCounter_,dropletCounterState_,rhi::ResourceState::UnorderedAccess);
    const u32 previousDropletGeneration=dropletGeneration_; ++dropletGeneration_; if(dropletGeneration_==0U) dropletGeneration_=1U;
    const f32 dropletDt=std::isfinite(deltaSeconds)?static_cast<f32>(std::clamp(deltaSeconds,0.0,1.0)):0.0F;
    std::array<u32,8U> dropletConstants{}; dropletConstants[0]=previousDropletGeneration; dropletConstants[1]=dropletGeneration_; dropletConstants[2]=generation_; dropletConstants[3]=MaximumDropletCount; dropletConstants[4]=Bits(dropletDt); dropletConstants[5]=Bits(static_cast<f32>(splashOriginDeltaMeters_.x)); dropletConstants[6]=Bits(static_cast<f32>(splashOriginDeltaMeters_.y)); dropletConstants[7]=Bits(static_cast<f32>(splashOriginDeltaMeters_.z));
    TransitionState(commands,*splashEvents_,splashEventState_,rhi::ResourceState::UnorderedAccess);
    commands.SetComputePipeline(*dropletSimulationPipeline_); commands.SetComputeConstants(dropletConstants); commands.SetComputeBuffer(0U,dropletSource); commands.SetComputeBuffer(1U,dropletDestination); commands.SetComputeBuffer(2U,*splashEvents_); commands.SetComputeBuffer(3U,*splashCounter_); commands.SetComputeBuffer(4U,*dropletCounter_);
    commands.Dispatch((std::max(MaximumDropletCount,MaximumSplashEventCount)+63U)/64U,1U,1U); commands.UavBarrier(dropletDestination); dropletCurrentIsA_=!dropletCurrentIsA_;

    // Refine droplet contact against the same resident physical pages. Water
    // re-entry appends a child splash event with allowSpray=false; terrain-only
    // contact simply retires the droplet.
    commands.SetComputePipeline(*dropletCollisionPipeline_);
    for(const auto& page:pages){ if(!page.IsValid()) continue; std::array<u32,12U> c{}; c[0]=page.face;c[1]=page.level;c[2]=page.tileX;c[3]=page.tileY;c[4]=page.resolution;c[5]=dropletGeneration_;c[6]=page.bodyIdentity[0];c[7]=page.bodyIdentity[1];c[8]=page.bodyIdentity[2];c[9]=page.bodyIdentity[3];c[10]=MaximumDropletCount;c[11]=Bits(dropletDt); commands.SetComputeConstants(c);commands.SetComputeBuffer(0U,dropletDestination);commands.SetComputeBuffer(1U,*page.samples);commands.SetComputeBuffer(2U,*splashEvents_);commands.SetComputeBuffer(3U,*splashCounter_);commands.Dispatch((MaximumDropletCount+63U)/64U,1U,1U);commands.UavBarrier(dropletDestination);commands.UavBarrier(*splashEvents_); }
    TransitionState(commands,dropletDestination,dropletDestinationState,rhi::ResourceState::ShaderResource);

    // Compact/age persistent splash state and append this generation's exact-once water-entry events.
    rhi::Buffer& splashSource=splashCurrentIsA_?*splashStateA_:*splashStateB_;'''
rep(p,needle,block)

# Bind droplet state for graphics and expose generations.
rep(p,
'''    commands.SetGraphicsBuffer(SplashGraphicsBufferSlot,splashCurrent);
}''',
'''    commands.SetGraphicsBuffer(SplashGraphicsBufferSlot,splashCurrent);
    rhi::Buffer& dropletCurrent=dropletCurrentIsA_?*dropletStateA_:*dropletStateB_;
    rhi::ResourceState& dropletState=dropletCurrentIsA_?dropletStateAState_:dropletStateBState_;
    TransitionState(commands,dropletCurrent,dropletState,rhi::ResourceState::ShaderResource);
    commands.SetGraphicsBuffer(DropletGraphicsBufferSlot,dropletCurrent);
}''')
rep(p,
'''    splashGeneration_ = 0U;
    splashCurrentIsA_ = true;
    splashOriginDeltaMeters_ = {};
    initialized_ = false;''',
'''    splashGeneration_ = 0U;
    dropletGeneration_ = 0U;
    splashCurrentIsA_ = true;
    dropletCurrentIsA_ = true;
    splashOriginDeltaMeters_ = {};
    initialized_ = false;''')
rep(p,
'''u32 VolumeParticleGpuState::SubmittedSpawnCount() const noexcept
{''',
'''u32 VolumeParticleGpuState::SplashGeneration() const noexcept
{
    return splashGeneration_;
}

u32 VolumeParticleGpuState::DropletGeneration() const noexcept
{
    return dropletGeneration_;
}

u32 VolumeParticleGpuState::SubmittedSpawnCount() const noexcept
{''')

# -----------------------------------------------------------------------------
# Renderer: droplet pipeline + exact current splash-generation filtering.
# -----------------------------------------------------------------------------
p='engine/volume_render/include/orbit/volume_render/VolumeParticleRenderer.hpp'
rep(p,
'''    std::unique_ptr<rhi::GraphicsPipeline> splashPipeline_;
''',
'''    std::unique_ptr<rhi::GraphicsPipeline> splashPipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> dropletPipeline_;
''')

p='engine/volume_render/src/VolumeParticleRenderer.cpp'
rep(p,
'''    if(e.generation==0u||e.baseScaleMeters<=0.0||e.lifetimeSeconds<=0.0||e.ageSeconds>=e.lifetimeSeconds){o.position=float4(2,2,1,1);o.uv=0;o.tint=0;o.impact=0;return o;}''',
'''    if(e.generation!=asuint(g.viewport.w)||e.baseScaleMeters<=0.0||e.lifetimeSeconds<=0.0||e.ageSeconds>=e.lifetimeSeconds){o.position=float4(2,2,1,1);o.uv=0;o.tint=0;o.impact=0;return o;}''')
# Insert droplet shaders before pixel shader.
marker='constexpr const char* kPixelShader = R"('
drop_render=r'''constexpr const char* kDropletVertexShader = R"(
struct Droplet { float3 positionMeters; float radiusMeters; float3 velocityMetersPerSecond; float ageSeconds; float3 tint; float lifetimeSeconds; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; uint4 bodyIdentity; uint generation; uint flags; uint2 reserved; };
[[vk::binding(4,0)]] StructuredBuffer<Droplet> g_droplets : register(t4);
struct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; }; [[vk::push_constant]] Push g;
struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float life:TEXCOORD2; };
float4 Project(float3 relative){ float3 f=normalize(g.forward.xyz),u=normalize(g.up.xyz),r=normalize(cross(f,u)),cu=normalize(cross(r,f)); float z=dot(relative,f); if(z<=g.projection.z||z>=g.projection.w)return float4(2,2,1,1); return float4(dot(relative,r)/(max(g.projection.x,0.001)*max(g.projection.y,0.001)),-dot(relative,cu)/max(g.projection.y,0.001),z*0.5,z); }
VSOutput main(uint vertexId:SV_VertexID){ static const float2 corners[6]={float2(-1,-1),float2(1,-1),float2(1,1),float2(-1,-1),float2(1,1),float2(-1,1)}; uint i=vertexId/6u,c=vertexId%6u; Droplet d=g_droplets[i]; VSOutput o; if(d.generation!=asuint(g.viewport.w)||d.lifetimeSeconds<=0.0||d.ageSeconds>=d.lifetimeSeconds){o.position=float4(2,2,1,1);o.uv=0;o.tint=0;o.life=0;return o;} float4 center=Project(d.positionMeters-g.camera.xyz); if(center.w<=0){o.position=center;o.uv=0;o.tint=0;o.life=0;return o;} float2 ndc=float2(2.0/max(g.viewport.x,1.0),2.0/max(g.viewport.y,1.0)); float px=max(d.radiusMeters,0.002)/max(center.w*max(g.projection.y,0.001),0.001)*max(g.viewport.y,1.0)*0.5; px=max(px,0.75); o.position=center;o.position.xy+=corners[c]*ndc*px*center.w;o.uv=corners[c];o.tint=max(d.tint,0.0);o.life=saturate(1.0-d.ageSeconds/max(d.lifetimeSeconds,0.001));return o; }
)";
constexpr const char* kDropletPixelShader = R"(
struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float life:TEXCOORD2; };
float4 main(VSOutput i):SV_Target0 { float r2=dot(i.uv,i.uv); if(r2>=1.0||i.life<=0.0) discard; float alpha=(1.0-smoothstep(0.2,1.0,r2))*i.life*0.82; float3 c=lerp(float3(0.70,0.84,0.94),float3(1,1,1),0.65)*i.tint; return float4(c,alpha); }
)";

'''
rep(p,marker,drop_render+marker)
rep(p,
'''    const auto splashVertex = compiler.Compile({.source=kSplashVertexShader,.entryPoint="main",.stage=shader::Stage::Vertex,.debug=false});
    const auto splashPixel = compiler.Compile({.source=kSplashPixelShader,.entryPoint="main",.stage=shader::Stage::Pixel,.debug=false});
    if (vertex.bytecode.empty() || pixel.bytecode.empty() || splashVertex.bytecode.empty() || splashPixel.bytecode.empty())''',
'''    const auto splashVertex = compiler.Compile({.source=kSplashVertexShader,.entryPoint="main",.stage=shader::Stage::Vertex,.debug=false});
    const auto splashPixel = compiler.Compile({.source=kSplashPixelShader,.entryPoint="main",.stage=shader::Stage::Pixel,.debug=false});
    const auto dropletVertex = compiler.Compile({.source=kDropletVertexShader,.entryPoint="main",.stage=shader::Stage::Vertex,.debug=false});
    const auto dropletPixel = compiler.Compile({.source=kDropletPixelShader,.entryPoint="main",.stage=shader::Stage::Pixel,.debug=false});
    if (vertex.bytecode.empty() || pixel.bytecode.empty() || splashVertex.bytecode.empty() || splashPixel.bytecode.empty() || dropletVertex.bytecode.empty() || dropletPixel.bytecode.empty())''')
rep(p,
'''    splashPipeline_ = device.CreateGraphicsPipeline({
        .vertexShader={.data=splashVertex.bytecode.data(),.size=splashVertex.bytecode.size()},
        .pixelShader={.data=splashPixel.bytecode.data(),.size=splashPixel.bytecode.size()},
        .vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=20U,.shaderResourceBuffers=4U,.sampledTextures=0U,
        .topology=rhi::PrimitiveTopology::TriangleList,.fillMode=rhi::FillMode::Solid,.cullMode=rhi::CullMode::None,
        .blendMode=rhi::BlendMode::Alpha,.depthCompare=rhi::DepthCompare::LessEqual,.depthTest=true,.depthWrite=false,
        .colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float},.colorAttachmentCount=1U
    });
}''',
'''    splashPipeline_ = device.CreateGraphicsPipeline({
        .vertexShader={.data=splashVertex.bytecode.data(),.size=splashVertex.bytecode.size()},
        .pixelShader={.data=splashPixel.bytecode.data(),.size=splashPixel.bytecode.size()},
        .vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=20U,.shaderResourceBuffers=4U,.sampledTextures=0U,
        .topology=rhi::PrimitiveTopology::TriangleList,.fillMode=rhi::FillMode::Solid,.cullMode=rhi::CullMode::None,
        .blendMode=rhi::BlendMode::Alpha,.depthCompare=rhi::DepthCompare::LessEqual,.depthTest=true,.depthWrite=false,
        .colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float},.colorAttachmentCount=1U
    });
    dropletPipeline_=device.CreateGraphicsPipeline({.vertexShader={.data=dropletVertex.bytecode.data(),.size=dropletVertex.bytecode.size()},.pixelShader={.data=dropletPixel.bytecode.data(),.size=dropletPixel.bytecode.size()},.vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=20U,.shaderResourceBuffers=5U,.sampledTextures=0U,.topology=rhi::PrimitiveTopology::TriangleList,.fillMode=rhi::FillMode::Solid,.cullMode=rhi::CullMode::None,.blendMode=rhi::BlendMode::Alpha,.depthCompare=rhi::DepthCompare::LessEqual,.depthTest=true,.depthWrite=false,.colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float},.colorAttachmentCount=1U});
}''')
# Draw: particle current gen, droplet current gen, splash current gen.
rep(p,
'''    commands.Draw(VolumeParticleGpuState::MaximumParticleCount * 6U);
    commands.SetGraphicsPipeline(*splashPipeline_);
    commands.SetGraphicsConstants(constants);
    state_.BindForGraphics(commands);
    commands.Draw(VolumeParticleGpuState::MaximumPersistentSplashCount * 6U);''',
'''    commands.Draw(VolumeParticleGpuState::MaximumParticleCount * 6U);
    constants[19] = state_.DropletGeneration();
    commands.SetGraphicsPipeline(*dropletPipeline_);
    commands.SetGraphicsConstants(constants);
    state_.BindForGraphics(commands);
    commands.Draw(VolumeParticleGpuState::MaximumDropletCount * 6U);
    constants[19] = state_.SplashGeneration();
    commands.SetGraphicsPipeline(*splashPipeline_);
    commands.SetGraphicsConstants(constants);
    state_.BindForGraphics(commands);
    commands.Draw(VolumeParticleGpuState::MaximumPersistentSplashCount * 6U);''')

# Regression constants/layout.
p='engine/studio_ui/tests/VolumeParticleRenderBridgeTests.cpp'
rep(p,
'''    Check(sizeof(volume_render::VolumeParticleGpuSplashEvent) == 48U);
    Check(volume_render::VolumeParticleGpuState::MaximumSplashEventCount == 4096U);
''',
'''    Check(sizeof(volume_render::VolumeParticleGpuSplashEvent) == 112U);
    Check(sizeof(volume_render::VolumeParticleGpuDropletState) == 112U);
    Check(volume_render::VolumeParticleGpuState::MaximumSplashEventCount == 4096U);
    Check(volume_render::VolumeParticleGpuState::MaximumDropletCount == 16384U);
    Check(volume_render::VolumeParticleGpuState::MaximumDropletsPerSplash == 8U);
''')
rep(p,
'''    Check(volume_render::VolumeParticleGpuState::SplashGraphicsBufferSlot == 3U);
''',
'''    Check(volume_render::VolumeParticleGpuState::SplashGraphicsBufferSlot == 3U);
    Check(volume_render::VolumeParticleGpuState::DropletGraphicsBufferSlot == 4U);
''')

print('M38 secondary droplet spray patch applied')
