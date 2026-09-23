#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/volume_render/VolumeParticleGpuBinding.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace orbit::volume_render
{
struct VolumeParticleGpuStateRecord
{
    math::Float3 positionMeters{};
    f32 authority{0.0F};
    math::Float3 velocityMetersPerSecond{};
    f32 density{0.0F};
    f32 emission{0.0F};
    f32 ageSeconds{0.0F};
    f32 lifetimeSeconds{0.0F};
    u32 generation{0U};
    math::Float4 reserved{};
};

static_assert(sizeof(VolumeParticleGpuStateRecord) == 64U);

struct VolumeParticleSimulationSettings
{
    f32 lifetimeSeconds{2.0F};
    f32 linearDragPerSecond{0.0F};
};

// Persistent GPU-only M38 particle state. Surviving particles are compacted
// into a ping-pong destination buffer with one atomic counter while the current
// simulation-step spawn packet is appended in the same dispatch. The CPU never
// reads the live count or particle records back.
class VolumeParticleGpuState
{
public:
    static constexpr u32 MaximumParticleCount = 65536U;
    static constexpr u32 ComputeBufferCount = 4U;
    static constexpr u32 GraphicsBufferSlot = 2U;

    VolumeParticleGpuState(
        rhi::Device& device,
        const shader::Compiler& compiler,
        u32 framesInFlight);

    void SetSpawns(
        std::span<const VolumeParticleGpuSpawn> spawns);

    // Advances the persistent state exactly once for a new simulation packet.
    // Existing state is rebased by previousOrigin-newOrigin before integration,
    // keeping float coordinates precise while the presentation origin moves.
    void Advance(
        rhi::CommandList& commands,
        u32 frameIndex,
        f64 deltaSeconds,
        math::Double3 previousOriginMeters,
        math::Double3 newOriginMeters,
        VolumeParticleSimulationSettings settings = {});

    void BindForGraphics(
        rhi::CommandList& commands);

    void Reset() noexcept;

    [[nodiscard]] u32 Generation() const noexcept;
    [[nodiscard]] u32 SubmittedSpawnCount() const noexcept;
    [[nodiscard]] rhi::Buffer& CurrentBuffer() noexcept;

private:
    using SpawnArray =
        std::array<
            VolumeParticleGpuSpawn,
            VolumeParticleGpuBinding::MaximumSpawnCount>;

    void InitializeState(
        rhi::CommandList& commands);

    SpawnArray spawnSnapshot_{};
    u32 spawnCount_{0U};
    u32 generation_{0U};
    bool initialized_{false};
    bool currentIsA_{true};

    std::unique_ptr<rhi::Buffer> stateA_;
    std::unique_ptr<rhi::Buffer> stateB_;
    std::unique_ptr<rhi::Buffer> zeroStateUpload_;
    std::unique_ptr<rhi::Buffer> counter_;
    std::unique_ptr<rhi::Buffer> zeroCounterUpload_;
    std::vector<std::unique_ptr<rhi::Buffer>> spawnBuffers_;
    std::unique_ptr<rhi::ComputePipeline> simulationPipeline_;
};
} // namespace orbit::volume_render
