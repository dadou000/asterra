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
    f32 reserved0{0.0F};
    f32 reserved1{0.0F};
    f32 reserved2{0.0F};
};

static_assert(sizeof(VolumeParticleGpuSpawn) == 48U);

// Frame-in-flight-safe upload seam for M38 particle spawn packets. This class
// deliberately owns no particle lifetime or simulation state: it transfers the
// authoritative per-simulation-step spawn packet to GPU-visible memory so a
// particle backend can consume it without a full volume-field readback.
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
