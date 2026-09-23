#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/volume_render/VolumeParticleGpuBinding.hpp>

#include <memory>
#include <span>

namespace orbit::volume_render
{
// Presentation-only renderer for M38 volume particle spawn packets. It does
// not integrate lifetime, forces, collision or motion; one authoritative spawn
// packet is rendered for the frame in which it is consumed. A future GPU
// particle simulator can replace this presentation layer without changing the
// volume-output contract or Studio handoff.
class VolumeParticleRenderer
{
public:
    VolumeParticleRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler,
        u32 framesInFlight);

    void SetSpawns(
        std::span<const VolumeParticleGpuSpawn> spawns);

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& sceneColor,
        rhi::Texture& depth,
        u32 width,
        u32 height,
        const render_view::CameraState& camera,
        math::Double3 cameraPositionRelativeToPresentationOriginMeters,
        u32 frameIndex,
        f32 radiusPixels = 3.0F);

    [[nodiscard]] u32 ActiveSpawnCount() const noexcept;

private:
    VolumeParticleGpuBinding binding_;
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
};
} // namespace orbit::volume_render
