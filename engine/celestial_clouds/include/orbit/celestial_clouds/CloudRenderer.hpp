#pragma once

#include <orbit/celestial_atmosphere/Atmosphere.hpp>
#include <orbit/celestial_atmosphere/AtmosphereRenderer.hpp>
#include <orbit/celestial_clouds/CloudField.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::celestial_clouds
{
// Cloud lab: when enabled the weather is replaced by one isolated cloud of the
// given type at a point on the body, with a taller shell, so its vertical
// development, shape and self-shadowing can be inspected in isolation.
struct CloudLab
{
    bool enabled{false};
    // Unit direction (body-fixed frame) of the cloud's centre.
    math::Double3 centerDirection{1.0, 0.0, 0.0};
    f32 radiusMeters{6000.0F};
    f32 type{1.0F};
    f32 coverage{0.85F};
    f32 cirrus{0.0F};
    f32 precipitation{0.3F};
    // Multiplies the vertical extent of the cloud shell.
    f32 heightScale{1.0F};
    // Life cycle: 0 towering cumulus, 0.3 growing cumulonimbus (cauliflower, no
    // anvil), 0.6 mature (glaciated top, anvil spreading), 0.9 dissipating (tower
    // collapsing, broad ragged anvil with mammatus).
    f32 maturity{0.6F};
    // 0 isolated single cell, 0.5 multicell cluster of cells of different ages,
    // 1 organised (large coherent updraft, wide anvil).
    f32 organisation{0.2F};
    // Multiplies the extinction (denser clouds shade themselves harder).
    f32 density{1.0F};
    // Coverage of a patchy thin-cirrus sheet on the anti-sun side of the cell (0 = none): it
    // sits where the storm's shadow falls, to see clouds shadowing cirrus.
    f32 cirrusSheet{0.0F};
    u32 seed{1U};
};

// Per-pixel ray-marched cloud shell for near-field and surface views, where
// the macro-globe appearance composite is not drawn. Reads the same GPU cloud
// field the orbital composite is built from, so both views agree on where
// the weather is. Composites sceneColor * T + L into target; run it before
// the atmosphere pass so aerial perspective is applied over the clouds.
class CloudRenderer
{
public:
    CloudRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);
    ~CloudRenderer();

    // One view's sun light volume: a cache of the optical depth towards the sun around that view's
    // camera (see kVolumeMath). It is tied to one camera (its anchor, toroidal window and voxel
    // generation), so every view that draws clouds needs its own: sharing one made two cameras
    // re-anchor it back and forth every frame, and no voxel ever stayed valid.
    struct LightVolume
    {
        std::unique_ptr<rhi::Buffer> buffer;
        math::Double3 anchor{};
        bool anchorSet{false};
        bool active{false};
        u32 generation{0U};
    };

    CloudRenderer(const CloudRenderer&) = delete;
    CloudRenderer& operator=(const CloudRenderer&) = delete;

    // Marches the cloud shell into 'target' (RGBA16F, usually half the scene
    // resolution) as premultiplied radiance in rgb and transmittance in alpha.
    // 'depth' is the full-resolution scene depth that bounds each ray. The
    // camera, sun and irradiance reuse the atmosphere view (body-fixed frame).
    // Only the first cloud layer is drawn.
    void Draw(
        rhi::CommandList& commands,
        const LightVolume& volume,
        rhi::Texture& depth,
        celestial_atmosphere::GpuAtmosphereLuts& luts,
        GpuCloudFieldProduct& field,
        rhi::Texture& target,
        u32 width,
        u32 height,
        f64 referenceRadiusMeters,
        const CloudLayerParameters& layer,
        const celestial_atmosphere::AtmosphereParameters& atmosphere,
        const celestial_atmosphere::AtmosphereRenderView& view,
        const CloudLab& lab = {},
        // Varies the march jitter frame to frame; the temporal Resolve averages the frames.
        u32 frameIndex = 0U,
        // Scales the crepuscular rays (sunlight removed from cloud-shadowed air); 0 = off.
        f32 godrayStrength = 1.0F,
        // > 0 draws a horizontal slice of the sun light volume at this altitude (m) as a heatmap.
        f32 volumeDebugAltitude = 0.0F);

    [[nodiscard]] std::unique_ptr<LightVolume> CreateLightVolume() const;

    // Refreshes the sun light volume: a compute pass that updates a few voxels per frame. Run it
    // before Draw in the same command list; Draw reads the volume when it is enabled.
    void UpdateLightVolume(
        rhi::CommandList& commands,
        LightVolume& volume,
        GpuCloudFieldProduct& field,
        f64 referenceRadiusMeters,
        const CloudLayerParameters& layer,
        const celestial_atmosphere::AtmosphereParameters& atmosphere,
        const celestial_atmosphere::AtmosphereRenderView& view,
        const CloudLab& lab,
        u32 frameIndex,
        bool enabled);

    // Sun transmittance (red channel, 1 = fully lit) through the cloud shell at
    // the surface each pixel sees, from the same density the march draws. The
    // target is a reduced-resolution RGBA16F texture sampled by the direct
    // lighting pass so terrain receives cloud shadows.
    void DrawShadow(
        rhi::CommandList& commands,
        rhi::Texture& depth,
        GpuCloudFieldProduct& field,
        rhi::Texture& target,
        u32 width,
        u32 height,
        f64 referenceRadiusMeters,
        const CloudLayerParameters& layer,
        const celestial_atmosphere::AtmosphereRenderView& view,
        const CloudLab& lab = {});

    // A camera as the temporal resolve needs it (body-fixed frame).
    struct CloudResolveView
    {
        math::Double3 cameraPositionMeters{};
        math::Float3 forward{0.0F, 0.0F, 1.0F};
        math::Float3 up{0.0F, 1.0F, 0.0F};
        f32 verticalFovRadians{1.0F};
        f32 aspect{1.0F};
    };

    // Temporal accumulation of the march: writes 'target' = history (reprojected from
    // 'previous' to 'now' through the cloud shell, clamped to the neighbourhood of
    // 'current') blended with 'current' by currentWeight. With no valid history the
    // current frame is copied. 'current', 'history' and 'target' share one size.
    void Resolve(
        rhi::CommandList& commands,
        rhi::Texture& current,
        rhi::Texture& history,
        rhi::Texture& target,
        u32 width,
        u32 height,
        f64 shellRadiusMeters,
        const CloudResolveView& now,
        const CloudResolveView& previous,
        bool historyValid,
        f32 currentWeight);

    // Blends the reduced-resolution result over 'target' (scene colour):
    // scene * T + L, bilinearly upsampled.
    void Composite(
        rhi::CommandList& commands,
        rhi::Texture& clouds,
        rhi::Texture& target,
        u32 width,
        u32 height);

private:
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> compositePipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> resolvePipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> shadowPipeline_;
    // Baked 32^3 tileable cellular noise (base shape + erosion octaves).
    std::unique_ptr<rhi::Buffer> noise_;
    // Sun light volume (see UpdateLightVolume): voxels, the pipeline that fills them and the
    // tangent-frame anchor that makes toroidal addressing valid.
    std::unique_ptr<rhi::ComputePipeline> volumePipeline_;
    rhi::Device* device_{nullptr};
};
} // namespace orbit::celestial_clouds
