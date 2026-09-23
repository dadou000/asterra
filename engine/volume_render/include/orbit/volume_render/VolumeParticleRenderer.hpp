#pragma once

#include <orbit/lighting/DirectLighting.hpp>
#include <orbit/lighting/LocalLightRegistry.hpp>
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
// GPU resident between simulation generations. Transparent presentation uses
// weighted OIT, samples read-only scene depth for soft intersections, and uses
// the same directional/local-light authority as Orbit's shared lighting path.
class VolumeParticleRenderer
{
public:
    static constexpr u32 MaximumLocalLightCount = 64U;
    static constexpr u32 ParticleLightGridResolution = 32U;

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
        u64 temporalHistoryKey,
        const lighting::DirectionalLight& stellarLight,
        std::span<const lighting::ResolvedLocalLight> localLights,
        f32 radiusPixels = 3.0F);

    void Reset() noexcept;

    [[nodiscard]] u32 Generation() const noexcept;
    [[nodiscard]] u32 SubmittedSpawnCount() const noexcept;
    [[nodiscard]] rhi::Buffer& ParticleLightGrid() noexcept;

private:
    struct OitTargets
    {
        std::unique_ptr<rhi::Texture> accumulation;
        std::unique_ptr<rhi::Texture> opticalDepth;
        std::unique_ptr<rhi::Texture> motionReject;
        std::unique_ptr<rhi::Texture> historyA;
        std::unique_ptr<rhi::Texture> historyB;
        std::unique_ptr<rhi::Buffer> localLights;
        rhi::ResourceState accumulationState{rhi::ResourceState::ShaderResource};
        rhi::ResourceState opticalDepthState{rhi::ResourceState::ShaderResource};
        rhi::ResourceState motionRejectState{rhi::ResourceState::ShaderResource};
        rhi::ResourceState historyAState{rhi::ResourceState::ShaderResource};
        rhi::ResourceState historyBState{rhi::ResourceState::ShaderResource};
        bool writeHistoryA{true};
        bool hasHistory{false};
        math::Double3 previousCameraPositionMeters{};
        math::Float3 previousForward{0.0F, 0.0F, 1.0F};
        u32 temporalSequence{0U};
    };

    [[nodiscard]] OitTargets& EnsureOitTargets(u32 width, u32 height, u32 frameIndex, u64 temporalHistoryKey);

    rhi::Device* device_{nullptr};
    u32 framesInFlight_{1U};
    VolumeParticleGpuState state_;
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> splashPipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> dropletPipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> oitTemporalPipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> oitCompositePipeline_;
    std::unique_ptr<rhi::ComputePipeline> particleLightGridPipeline_;
    std::unique_ptr<rhi::Buffer> particleLightGrid_;
    std::unique_ptr<rhi::Buffer> zeroParticleLightGridUpload_;
    rhi::ResourceState particleLightGridState_{rhi::ResourceState::CopyDestination};
    std::map<std::pair<u64, u64>, std::vector<OitTargets>> oitTargets_;
    std::vector<VolumeParticleTerrainCollisionPage> terrainCollisionPages_;
};
} // namespace orbit::volume_render