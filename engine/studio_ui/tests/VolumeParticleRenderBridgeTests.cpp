#include <orbit/studio_session/VolumeParticleOutputState.hpp>
#include <orbit/studio_ui/VolumeParticleRenderBridge.hpp>
#include <orbit/volume_render/VolumeParticleGpuState.hpp>
#include <orbit/volume_render/VolumeParticleRenderer.hpp>

#include <cstdlib>
#include <iostream>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "M38 particle render bridge regression failed.\n";
        std::exit(1);
    }
}
}

int main()
{
    using namespace orbit;

    static_assert(requires(
        volume_render::VolumeParticleGpuState& state,
        rhi::CommandList& commands)
    {
        state.BuildVisibleDrawLists(
            commands,
            math::Float3{},
            math::Float3{},
            math::Float3{},
            1.0F,
            1.0F,
            0.1F,
            1000.0F);
    });

    static_assert(requires(
        volume_render::VolumeParticleRenderer& renderer,
        rhi::CommandList& commands,
        rhi::Texture& color,
        rhi::Texture& depth,
        const render_view::CameraState& camera)
    {
        renderer.Draw(
            commands,
            color,
            depth,
            1920U,
            1080U,
            camera,
            math::Double3{},
            0U,
            0x12345678ULL,
            3.0F);
    });

    Check(sizeof(volume_render::VolumeParticleGpuSplashEvent) == 112U);
    Check(sizeof(volume_render::VolumeParticleGpuDropletState) == 112U);
    Check(volume_render::VolumeParticleGpuState::MaximumSplashEventCount == 4096U);
    Check(volume_render::VolumeParticleGpuState::MaximumDropletCount == 16384U);
    Check(volume_render::VolumeParticleGpuState::MaximumDropletsPerSplash == 8U);
    Check(volume_render::VolumeParticleGpuState::MaximumDropletCount >=
          volume_render::VolumeParticleGpuState::MaximumSplashEventCount * 4U);
    Check(volume_render::VolumeParticleGpuState::MaximumPersistentSplashCount == 8192U);
    Check(volume_render::VolumeParticleGpuState::MaximumPersistentSplashCount >=
          volume_render::VolumeParticleGpuState::MaximumSplashEventCount);
    Check(sizeof(volume_render::VolumeParticleGpuSplashState) == 64U);
    Check(volume_render::VolumeParticleGpuState::SplashGraphicsBufferSlot == 3U);
    Check(volume_render::VolumeParticleGpuState::DropletGraphicsBufferSlot == 4U);
    Check(volume_render::VolumeParticleGpuState::ParticleActiveIndexGraphicsBufferSlot == 5U);
    Check(volume_render::VolumeParticleGpuState::SplashActiveIndexGraphicsBufferSlot == 6U);
    Check(volume_render::VolumeParticleGpuState::DropletActiveIndexGraphicsBufferSlot == 7U);
    Check(volume_render::VolumeParticleGpuState::ParticleIndirectOffsetBytes == 0U);
    Check(volume_render::VolumeParticleGpuState::SplashIndirectOffsetBytes == 16U);
    Check(volume_render::VolumeParticleGpuState::DropletIndirectOffsetBytes == 32U);
    Check(static_cast<u32>(rhi::BlendMode::Additive) != static_cast<u32>(rhi::BlendMode::Alpha));
    Check(rhi::TextureFormatBytesPerTexel(rhi::TextureFormat::RGBA16_Float) == 8U);
    Check(rhi::TextureFormatBytesPerTexel(rhi::TextureFormat::R16_Float) == 2U);
    Check(volume_render::VolumeParticleRenderer::MaximumLocalLightCount == 64U);
    Check(volume_render::VolumeParticleRenderer::ParticleLightGridResolution == 32U);

    const universe::BodyId body{
        .high = 0x1122334455667788ULL,
        .low = 0x99AABBCCDDEEFF00ULL
    };

    std::vector<studio_session::VolumeParticleRuntimeEvent> events;
    events.push_back({
        .sourceVolume = {.high = 1U, .low = 1U},
        .request = {
            .eventId = 3U,
            .positionMeters = {1000000001.25, -2.5, 4.0},
            .velocity = {1.0F, 2.0F, 3.0F},
            .authority = 0.25F,
            .density = 0.5F,
            .emission = 0.1F,
            .lifetimeSeconds = 5.0F,
            .linearDragPerSecond = 0.4F,
            .radiusMeters = 0.5F,
            .emissionScale = 3.0F,
            .baseColor = {0.2F, 0.3F, 0.4F},
            .emissionColor = {1.0F, 0.5F, 0.1F},
            .gravityMode = world_model::VolumeParticleGravityMode::OwningBody,
            .gravityScale = 0.75F,
            .collisionMode = world_model::VolumeParticleCollisionMode::Bounce,
            .restitution = 0.6F,
            .waterDensityRatio = 0.8F,
            .waterDragPerSecond = 7.5F,
            .waterBuoyancyScale = 1.2F,
            .killOnWaterImmersion = true,
            .splashOnWaterEntry = true,
            .waterSplashScale = 2.5F
        },
        .physics = {
            .body = body,
            .gravitationalParameterM3PerS2 = 3.986004418e14,
            .gravitySofteningMeters = 10.0,
            .surfaceRadiiMeters = {6378137.0, 6378137.0, 6356752.3},
            .hasPhysicalSurface = true
        }});
    events.push_back({
        .sourceVolume = {.high = 1U, .low = 2U},
        .request = {
            .eventId = 2U,
            .positionMeters = {1000000002.5, 0.0, 0.0},
            .authority = 0.9F,
            .density = 0.3F,
            .emission = 2.0F
        }});
    events.push_back({
        .sourceVolume = {.high = 1U, .low = 3U},
        .request = {
            .eventId = 1U,
            .positionMeters = {1000000003.5, 0.0, 0.0},
            .authority = 0.9F,
            .density = -4.0F,
            .emission = -1.0F
        }});

    const math::Double3 origin{1000000000.0, 0.0, 0.0};
    const auto batch = studio_ui::BuildVolumeParticleRenderBatch(events, origin, 2U);

    Check(batch.size() == 2U);
    Check(batch[0].positionMeters.x == 3.5F);
    Check(batch[1].positionMeters.x == 2.5F);
    Check(batch[0].authority == 0.9F);
    Check(batch[1].authority == 0.9F);
    Check(batch[0].density == 0.0F);
    Check(batch[0].emission == 0.0F);
    Check(batch[1].emission == 2.0F);

    const auto full = studio_ui::BuildVolumeParticleRenderBatch(events, origin, 3U);
    Check(full.size() == 3U);
    Check(full[0].positionMeters.x == 1.25F);
    Check(full[0].velocityMetersPerSecond.x == 1.0F);
    Check(full[0].velocityMetersPerSecond.y == 2.0F);
    Check(full[0].velocityMetersPerSecond.z == 3.0F);

    Check(full[0].lifetimeSeconds == 5.0F);
    Check(full[0].linearDragPerSecond == 0.4F);
    Check(full[0].radiusMeters == 0.5F);
    Check(full[0].emissionScale == 3.0F);
    Check(full[0].gravityScale == 0.75F);
    Check(full[0].restitution == 0.6F);
    Check(full[0].behaviorFlags == 125U);
    Check(full[0].waterDensityRatio == 0.8F);
    Check(full[0].waterDragPerSecond == 7.5F);
    Check(full[0].waterBuoyancyScale == 1.2F);
    Check(full[0].waterSplashScale == 2.5F);
    Check((full[0].behaviorFlags & (1U << 4U)) != 0U);
    Check((full[0].behaviorFlags & (1U << 5U)) != 0U);
    Check((full[0].behaviorFlags & (1U << 6U)) != 0U);
    Check((full[0].behaviorFlags & (1U << 30U)) == 0U);
    Check((full[0].behaviorFlags & (1U << 31U)) == 0U);
    Check(full[0].baseColor.x == 0.2F);
    Check(full[0].baseColor.y == 0.3F);
    Check(full[0].baseColor.z == 0.4F);
    Check(full[0].emissionColor.x == 1.0F);
    Check(full[0].emissionColor.y == 0.5F);
    Check(full[0].emissionColor.z == 0.1F);

    Check(full[0].bodyCenterMeters.x == -1000000000.0F);
    Check(full[0].bodyCenterMeters.y == 0.0F);
    Check(full[0].bodyCenterMeters.z == 0.0F);
    Check(full[0].gravitationalParameterM3PerS2 > 3.98e14F);
    Check(full[0].gravitySofteningMeters == 10.0F);
    Check(full[0].surfaceRadiiMeters.x == 6378137.0F);
    Check(full[0].surfaceRadiiMeters.z > 6356752.0F);

    const auto bodyWords = studio_ui::VolumeParticleBodyIdentityWords(body);
    Check(full[0].bodyIdentity == bodyWords);
    Check(bodyWords[0] == 0x55667788U);
    Check(bodyWords[1] == 0x11223344U);
    Check(bodyWords[2] == 0xDDEEFF00U);
    Check(bodyWords[3] == 0x99AABBCCU);

    studio_session::VolumeParticleOutputState output;
    Check(output.Diagnostics().generation == 0U);
    output.Consume({});
    Check(output.Diagnostics().generation == 1U);
    Check(output.Events().empty());
    output.Consume({});
    Check(output.Diagnostics().generation == 2U);
    output.Clear();
    Check(output.Diagnostics().generation == 0U);
    Check(output.Events().empty());

    return 0;
}