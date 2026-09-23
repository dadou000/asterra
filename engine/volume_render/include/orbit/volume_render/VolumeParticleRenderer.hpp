#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/volume_render/VolumeParticleGpuState.hpp>

#include <array>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace orbit::volume_render
{
// Persistent M38 GPU particle renderer. CPU input is limited to compact spawn
// and terrain-page metadata; particle state, integration and collision remain
// GPU resident between simulation generations.
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

    void UpdateTerrainCollisionPages(
        const std::array<u32, 4U>& bodyIdentity,
        std::span<const VolumeParticleTerrainCollisionPage> pages);

    [[nodiscard]] std::vector<VolumeParticleTerrainCollisionPage>
    TerrainCollisionPagesSnapshot() const;

    void ApplyTerrainCollision(
        rhi::CommandList& commands,
        std::span<const VolumeParticleTerrainCollisionPage> pages,
        f64 deltaSeconds);

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

    void Reset() noexcept;

    [[nodiscard]] u32 Generation() const noexcept;
    [[nodiscard]] u32 SubmittedSpawnCount() const noexcept;

private:
    struct OitTargets
    {
        std::unique_ptr<rhi::Texture> accumulation;
        std::unique_ptr<rhi::Texture> opticalDepth;
        rhi::ResourceState accumulationState{rhi::ResourceState::ShaderResource};
        rhi::ResourceState opticalDepthState{rhi::ResourceState::ShaderResource};
    };

    [[nodiscard]] OitTargets& EnsureOitTargets(u32 width, u32 height, u32 frameIndex);

    rhi::Device* device_{nullptr};
    u32 framesInFlight_{1U};
    VolumeParticleGpuState state_;
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> splashPipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> dropletPipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> oitCompositePipeline_;
    std::map<u64, std::vector<OitTargets>> oitTargets_;
    std::vector<VolumeParticleTerrainCollisionPage> terrainCollisionPages_;
};
} // namespace orbit::volume_render
