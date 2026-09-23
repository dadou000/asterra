#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace orbit::volume_render
{
struct VolumeParticleGpuSpawn
{
    math::Float3 positionMeters{};
    f32 authority{0.0F};
    math::Float3 velocityMetersPerSecond{};
    f32 density{0.0F};
    f32 emission{0.0F};
    f32 lifetimeSeconds{2.0F};
    f32 linearDragPerSecond{0.0F};
    f32 radiusMeters{0.08F};
    f32 emissionScale{1.0F};
    f32 gravityScale{1.0F};
    f32 restitution{0.25F};
    u32 behaviorFlags{0U};
    math::Float3 baseColor{1.0F, 1.0F, 1.0F};
    f32 reserved0{0.0F};
    math::Float3 emissionColor{1.0F, 0.32F, 0.06F};
    f32 reserved1{0.0F};

    // Owning-body physics authority. Positions are body-local, therefore the
    // gravity center is exactly the body-frame origin and does not need to be
    // carried per particle.
    math::Float3 surfaceRadiiMeters{};
    f32 gravitationalParameterM3PerS2{0.0F};
    f32 gravitySofteningMeters{0.0F};
    f32 physicalSurfaceEnabled{0.0F};
    f32 reserved2{0.0F};
    f32 reserved3{0.0F};
};

static_assert(sizeof(VolumeParticleGpuSpawn) == 128U);

class VolumeParticleGpuBinding
{
public:
    static constexpr u32 MaximumSpawnCount = 4096U;
    static constexpr u32 GraphicsBufferSlot = 2U;

    VolumeParticleGpuBinding(
        rhi::Device& device,
        u32 framesInFlight);

    void Set(
        std::span<const VolumeParticleGpuSpawn> spawns);

    void Bind(
        rhi::CommandList& commands,
        u32 frameIndex);

    [[nodiscard]] u32 ActiveSpawnCount() const noexcept;

private:
    using SpawnArray =
        std::array<VolumeParticleGpuSpawn, MaximumSpawnCount>;

    SpawnArray snapshot_{};
    u32 activeSpawnCount_{0U};
    std::vector<std::unique_ptr<rhi::Buffer>> frameBuffers_;
};
} // namespace orbit::volume_render
