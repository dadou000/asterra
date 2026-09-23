#include <orbit/volume_render/VolumeParticleGpuState.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace orbit::volume_render
{
namespace
{
constexpr const char* kSimulationShader = R"(
struct Spawn
{
    float3 positionMeters;
    float authority;
    float3 velocityMetersPerSecond;
    float density;
    float emission;
    float reserved0;
    float reserved1;
    float reserved2;
};

struct Particle
{
    float3 positionMeters;
    float authority;
    float3 velocityMetersPerSecond;
    float density;
    float emission;
    float ageSeconds;
    float lifetimeSeconds;
    uint generation;
    float4 reserved;
};

[[vk::binding(0, 0)]]
RWStructuredBuffer<Particle> g_source : register(u0);
[[vk::binding(1, 0)]]
RWStructuredBuffer<Particle> g_destination : register(u1);
[[vk::binding(2, 0)]]
RWStructuredBuffer<Spawn> g_spawns : register(u2);
[[vk::binding(3, 0)]]
RWStructuredBuffer<uint> g_counter : register(u3);

struct Push
{
    uint4 counts;
    float4 timing;
    float4 originDelta;
};

[[vk::push_constant]]
Push g;

void AppendParticle(Particle particle)
{
    uint destinationIndex = 0u;
    InterlockedAdd(g_counter[0], 1u, destinationIndex);
    if (destinationIndex < g.counts.w)
    {
        particle.generation = g.counts.y;
        g_destination[destinationIndex] = particle;
    }
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    const uint sourceGeneration = g.counts.x;
    const uint spawnCount = g.counts.z;
    const uint capacity = g.counts.w;
    const float dt = max(g.timing.x, 0.0);
    const float spawnLifetime = max(g.timing.y, 0.001);
    const float drag = max(g.timing.z, 0.0);

    if (index < capacity)
    {
        Particle particle = g_source[index];
        if (particle.generation == sourceGeneration &&
            particle.lifetimeSeconds > 0.0)
        {
            particle.ageSeconds += dt;
            if (particle.ageSeconds < particle.lifetimeSeconds)
            {
                particle.positionMeters += g.originDelta.xyz;
                const float dragScale = exp(-drag * dt);
                particle.velocityMetersPerSecond *= dragScale;
                particle.positionMeters +=
                    particle.velocityMetersPerSecond * dt;
                AppendParticle(particle);
            }
        }
    }

    if (index < spawnCount)
    {
        const Spawn spawn = g_spawns[index];
        Particle particle;
        particle.positionMeters = spawn.positionMeters;
        particle.authority = max(spawn.authority, 0.0);
        particle.velocityMetersPerSecond = spawn.velocityMetersPerSecond;
        particle.density = max(spawn.density, 0.0);
        particle.emission = max(spawn.emission, 0.0);
        particle.ageSeconds = 0.0;
        particle.lifetimeSeconds = spawnLifetime;
        particle.generation = g.counts.y;
        particle.reserved = 0.0;
        AppendParticle(particle);
    }
}
)";

[[nodiscard]] u32 Bits(const f32 value) noexcept
{
    return std::bit_cast<u32>(value);
}
} // namespace

VolumeParticleGpuState::VolumeParticleGpuState(
    rhi::Device& device,
    const shader::Compiler& compiler,
    const u32 framesInFlight)
{
    if (framesInFlight == 0U)
    {
        throw std::invalid_argument(
            "Orbit M38 GPU particle state requires at least one frame in flight.");
    }

    constexpr u64 stateBytes =
        static_cast<u64>(MaximumParticleCount) *
        sizeof(VolumeParticleGpuStateRecord);
    constexpr u64 spawnBytes =
        static_cast<u64>(VolumeParticleGpuBinding::MaximumSpawnCount) *
        sizeof(VolumeParticleGpuSpawn);

    const rhi::BufferDesc stateDesc{
        .sizeBytes = stateBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination
    };
    stateA_ = device.CreateBuffer(stateDesc);
    stateB_ = device.CreateBuffer(stateDesc);

    zeroStateUpload_ = device.CreateBuffer({
        .sizeBytes = stateBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::CopySource
    });
    {
        std::byte* mapped = zeroStateUpload_->Map();
        std::memset(mapped, 0, static_cast<std::size_t>(stateBytes));
        zeroStateUpload_->Unmap();
    }

    counter_ = device.CreateBuffer({
        .sizeBytes = sizeof(u32),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination
    });
    zeroCounterUpload_ = device.CreateBuffer({
        .sizeBytes = sizeof(u32),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::CopySource
    });
    {
        std::byte* mapped = zeroCounterUpload_->Map();
        std::memset(mapped, 0, sizeof(u32));
        zeroCounterUpload_->Unmap();
    }

    spawnBuffers_.reserve(framesInFlight);
    for (u32 frame = 0U; frame < framesInFlight; ++frame)
    {
        spawnBuffers_.push_back(device.CreateBuffer({
            .sizeBytes = spawnBytes,
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::UnorderedAccess
        }));
    }

    const auto compute = compiler.Compile({
        .source = kSimulationShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    });
    if (compute.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile the M38 GPU particle simulation shader.");
    }

    simulationPipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = compute.bytecode.data(),
            .size = compute.bytecode.size()
        },
        .pushConstantDwords = 12U,
        .shaderResourceBuffers = ComputeBufferCount,
        .storageTextures = 0U,
        .sampledTextures = 0U,
        .accelerationStructures = 0U
    });
}

void VolumeParticleGpuState::SetSpawns(
    const std::span<const VolumeParticleGpuSpawn> spawns)
{
    spawnSnapshot_.fill({});
    spawnCount_ = static_cast<u32>(
        std::min<std::size_t>(
            spawns.size(),
            VolumeParticleGpuBinding::MaximumSpawnCount));
    std::copy_n(spawns.begin(), spawnCount_, spawnSnapshot_.begin());
}

void VolumeParticleGpuState::TransitionState(
    rhi::CommandList& commands,
    rhi::Buffer& buffer,
    rhi::ResourceState& tracked,
    const rhi::ResourceState desired)
{
    if (tracked != desired)
    {
        commands.Transition(buffer, tracked, desired);
        tracked = desired;
    }
}

void VolumeParticleGpuState::InitializeState(
    rhi::CommandList& commands)
{
    TransitionState(
        commands, *stateA_, stateAState_, rhi::ResourceState::CopyDestination);
    TransitionState(
        commands, *stateB_, stateBState_, rhi::ResourceState::CopyDestination);
    TransitionState(
        commands, *counter_, counterState_, rhi::ResourceState::CopyDestination);

    commands.CopyBuffer(
        *zeroStateUpload_, 0U, *stateA_, 0U, stateA_->SizeBytes());
    commands.CopyBuffer(
        *zeroStateUpload_, 0U, *stateB_, 0U, stateB_->SizeBytes());
    commands.CopyBuffer(
        *zeroCounterUpload_, 0U, *counter_, 0U, sizeof(u32));

    TransitionState(
        commands, *stateA_, stateAState_, rhi::ResourceState::UnorderedAccess);
    TransitionState(
        commands, *stateB_, stateBState_, rhi::ResourceState::UnorderedAccess);
    TransitionState(
        commands, *counter_, counterState_, rhi::ResourceState::UnorderedAccess);

    currentIsA_ = true;
    generation_ = 0U;
    initialized_ = true;
}

void VolumeParticleGpuState::Advance(
    rhi::CommandList& commands,
    const u32 frameIndex,
    const f64 deltaSeconds,
    const math::Double3 previousOriginMeters,
    const math::Double3 newOriginMeters,
    const VolumeParticleSimulationSettings settings)
{
    if (frameIndex >= spawnBuffers_.size())
    {
        throw std::out_of_range(
            "Orbit M38 particle simulation frame index exceeds frames in flight.");
    }

    if (!initialized_)
    {
        InitializeState(commands);
    }

    rhi::Buffer& spawnBuffer = *spawnBuffers_[frameIndex];
    {
        std::byte* mapped = spawnBuffer.Map();
        std::memcpy(mapped, spawnSnapshot_.data(), sizeof(spawnSnapshot_));
        spawnBuffer.Unmap();
    }

    TransitionState(
        commands, *counter_, counterState_, rhi::ResourceState::CopyDestination);
    commands.CopyBuffer(
        *zeroCounterUpload_, 0U, *counter_, 0U, sizeof(u32));
    TransitionState(
        commands, *counter_, counterState_, rhi::ResourceState::UnorderedAccess);

    rhi::Buffer& source = currentIsA_ ? *stateA_ : *stateB_;
    rhi::Buffer& destination = currentIsA_ ? *stateB_ : *stateA_;
    rhi::ResourceState& sourceState = currentIsA_ ? stateAState_ : stateBState_;
    rhi::ResourceState& destinationState = currentIsA_ ? stateBState_ : stateAState_;

    TransitionState(
        commands, source, sourceState, rhi::ResourceState::UnorderedAccess);
    TransitionState(
        commands, destination, destinationState, rhi::ResourceState::UnorderedAccess);

    const u32 sourceGeneration = generation_;
    ++generation_;
    if (generation_ == 0U)
    {
        generation_ = 1U;
    }

    const f32 dt = std::isfinite(deltaSeconds)
        ? static_cast<f32>(std::clamp(deltaSeconds, 0.0, 1.0))
        : 0.0F;
    const f32 lifetime = std::isfinite(settings.lifetimeSeconds)
        ? std::max(settings.lifetimeSeconds, 0.001F)
        : 2.0F;
    const f32 drag = std::isfinite(settings.linearDragPerSecond)
        ? std::max(settings.linearDragPerSecond, 0.0F)
        : 0.0F;

    const math::Double3 originDelta{
        previousOriginMeters.x - newOriginMeters.x,
        previousOriginMeters.y - newOriginMeters.y,
        previousOriginMeters.z - newOriginMeters.z
    };

    std::array<u32, 12U> constants{};
    constants[0] = sourceGeneration;
    constants[1] = generation_;
    constants[2] = spawnCount_;
    constants[3] = MaximumParticleCount;
    constants[4] = Bits(dt);
    constants[5] = Bits(lifetime);
    constants[6] = Bits(drag);
    constants[8] = Bits(static_cast<f32>(originDelta.x));
    constants[9] = Bits(static_cast<f32>(originDelta.y));
    constants[10] = Bits(static_cast<f32>(originDelta.z));

    commands.SetComputePipeline(*simulationPipeline_);
    commands.SetComputeConstants(constants);
    commands.SetComputeBuffer(0U, source);
    commands.SetComputeBuffer(1U, destination);
    commands.SetComputeBuffer(2U, spawnBuffer);
    commands.SetComputeBuffer(3U, *counter_);
    commands.Dispatch((MaximumParticleCount + 63U) / 64U, 1U, 1U);
    commands.UavBarrier(destination);

    TransitionState(
        commands,
        destination,
        destinationState,
        rhi::ResourceState::ShaderResource);

    currentIsA_ = !currentIsA_;
}

void VolumeParticleGpuState::BindForGraphics(
    rhi::CommandList& commands)
{
    rhi::Buffer& current = CurrentBuffer();
    rhi::ResourceState& state = currentIsA_ ? stateAState_ : stateBState_;
    TransitionState(
        commands, current, state, rhi::ResourceState::ShaderResource);
    commands.SetGraphicsBuffer(GraphicsBufferSlot, current);
}

void VolumeParticleGpuState::Reset() noexcept
{
    spawnSnapshot_.fill({});
    spawnCount_ = 0U;
    generation_ = 0U;
    initialized_ = false;
}

u32 VolumeParticleGpuState::Generation() const noexcept
{
    return generation_;
}

u32 VolumeParticleGpuState::SubmittedSpawnCount() const noexcept
{
    return spawnCount_;
}

rhi::Buffer& VolumeParticleGpuState::CurrentBuffer() noexcept
{
    return currentIsA_ ? *stateA_ : *stateB_;
}
} // namespace orbit::volume_render
