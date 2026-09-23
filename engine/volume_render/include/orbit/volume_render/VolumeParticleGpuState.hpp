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
    f32 linearDragPerSecond{0.0F};
    f32 radiusMeters{0.08F};
    f32 emissionScale{1.0F};
    f32 gravityScale{1.0F};
    f32 restitution{0.25F};
    math::Float3 baseColor{1.0F, 1.0F, 1.0F};
    u32 behaviorFlags{0U};
    math::Float3 emissionColor{1.0F, 0.32F, 0.06F};
    u32 generation{0U};
    math::Float3 bodyCenterMeters{};
    f32 gravitationalParameterM3PerS2{0.0F};
    math::Float3 surfaceRadiiMeters{};
    f32 gravitySofteningMeters{0.0F};
    f32 waterDensityRatio{1.0F};
    f32 waterDragPerSecond{0.0F};
    f32 waterBuoyancyScale{1.0F};
    f32 waterSplashScale{1.0F};
    std::array<u32, 4U> bodyIdentity{};
};

static_assert(sizeof(VolumeParticleGpuStateRecord) == 160U);

struct VolumeParticleGpuSplashEvent
{
    math::Float3 positionMeters{};
    f32 scaleMeters{0.0F};
    math::Float3 normal{};
    f32 impactSpeedMetersPerSecond{0.0F};
    math::Float3 tint{1.0F, 1.0F, 1.0F};
    u32 generation{0U};
};
static_assert(sizeof(VolumeParticleGpuSplashEvent) == 48U);

struct VolumeParticleSimulationSettings
{
    f32 lifetimeSeconds{2.0F};
    f32 linearDragPerSecond{0.0F};
};

// One resident M12/M26 physical-surface page exposed to the particle collision
// pass. `samples` stays GPU resident; only compact tile metadata crosses CPU.
struct VolumeParticleTerrainCollisionPage
{
    std::shared_ptr<rhi::Buffer> samples;
    u32 resolution{0U};
    u32 face{0U};
    u32 level{0U};
    u32 tileX{0U};
    u32 tileY{0U};
    std::array<u32, 4U> bodyIdentity{};

    [[nodiscard]] bool IsValid() const noexcept
    {
        return
            samples != nullptr &&
            resolution >= 2U &&
            samples->SizeBytes() >=
                static_cast<u64>(resolution) *
                resolution *
                2U * sizeof(f32);
    }
};

class VolumeParticleGpuState
{
public:
    static constexpr u32 MaximumParticleCount = 65536U;
    static constexpr u32 MaximumSplashEventCount = 4096U;
    static constexpr u32 ComputeBufferCount = 4U;
    static constexpr u32 GraphicsBufferSlot = 2U;
    static constexpr u32 SplashGraphicsBufferSlot = 3U;

    VolumeParticleGpuState(
        rhi::Device& device,
        const shader::Compiler& compiler,
        u32 framesInFlight);

    void SetSpawns(std::span<const VolumeParticleGpuSpawn> spawns);

    void Advance(
        rhi::CommandList& commands,
        u32 frameIndex,
        f64 deltaSeconds,
        math::Double3 previousOriginMeters,
        math::Double3 newOriginMeters,
        VolumeParticleSimulationSettings settings = {});

    // Refines the cheap reference-ellipsoid collision using the actual
    // resident physical terrain pages. The state stays on GPU and is modified
    // in place after integration; pages outside coverage are simply skipped.
    void ApplyTerrainCollision(
        rhi::CommandList& commands,
        std::span<const VolumeParticleTerrainCollisionPage> pages,
        f64 deltaSeconds);

    void BindForGraphics(rhi::CommandList& commands);
    void Reset() noexcept;

    [[nodiscard]] u32 Generation() const noexcept;
    [[nodiscard]] u32 SubmittedSpawnCount() const noexcept;
    [[nodiscard]] rhi::Buffer& CurrentBuffer() noexcept;

private:
    using SpawnArray = std::array<
        VolumeParticleGpuSpawn,
        VolumeParticleGpuBinding::MaximumSpawnCount>;

    void InitializeState(rhi::CommandList& commands);
    void TransitionState(
        rhi::CommandList& commands,
        rhi::Buffer& buffer,
        rhi::ResourceState& tracked,
        rhi::ResourceState desired);

    SpawnArray spawnSnapshot_{};
    u32 spawnCount_{0U};
    u32 generation_{0U};
    bool initialized_{false};
    bool currentIsA_{true};

    rhi::ResourceState stateAState_{rhi::ResourceState::CopyDestination};
    rhi::ResourceState stateBState_{rhi::ResourceState::CopyDestination};
    rhi::ResourceState counterState_{rhi::ResourceState::CopyDestination};
    rhi::ResourceState splashEventState_{rhi::ResourceState::UnorderedAccess};
    rhi::ResourceState splashCounterState_{rhi::ResourceState::CopyDestination};

    std::unique_ptr<rhi::Buffer> stateA_;
    std::unique_ptr<rhi::Buffer> stateB_;
    std::unique_ptr<rhi::Buffer> zeroStateUpload_;
    std::unique_ptr<rhi::Buffer> counter_;
    std::unique_ptr<rhi::Buffer> zeroCounterUpload_;
    std::unique_ptr<rhi::Buffer> splashEvents_;
    std::unique_ptr<rhi::Buffer> splashCounter_;
    std::unique_ptr<rhi::Buffer> zeroSplashCounterUpload_;
    std::vector<std::unique_ptr<rhi::Buffer>> spawnBuffers_;
    std::unique_ptr<rhi::ComputePipeline> simulationPipeline_;
    std::unique_ptr<rhi::ComputePipeline> terrainCollisionPipeline_;
};
} // namespace orbit::volume_render
