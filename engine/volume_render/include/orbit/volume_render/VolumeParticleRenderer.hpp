#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/volume_render/VolumeParticleGpuState.hpp>

#include <memory>
#include <span>

namespace orbit::volume_render
{
// Persistent M38 GPU particle renderer. CPU input is limited to the compact
// authoritative spawn packet; lifetime, velocity integration and compaction
// remain GPU resident between simulation generations.
class VolumeParticleRenderer
{
public:
    VolumeParticleRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler,
        u32 framesInFlight);

    void SetSpawns(
        std::span<const VolumeParticleGpuSpawn> spawns);

    void Advance(
        rhi::CommandList& commands,
        u32 frameIndex,
        f64 deltaSeconds,
        math::Double3 previousOriginMeters,
        math::Double3 newOriginMeters,
        VolumeParticleSimulationSettings settings = {});

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& sceneColor,
        rhi::Texture& depth,
        u32 width,
        u32 height,
        const render_view::CameraState& camera,
        math::Double3 cameraPositionRelativeToPresentationOriginMeters,
        f32 radiusPixels = 3.0F);

    void Reset() noexcept;

    [[nodiscard]] u32 Generation() const noexcept;
    [[nodiscard]] u32 SubmittedSpawnCount() const noexcept;

private:
    VolumeParticleGpuState state_;
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
};
} // namespace orbit::volume_render
