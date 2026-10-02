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

    CloudRenderer(const CloudRenderer&) = delete;
    CloudRenderer& operator=(const CloudRenderer&) = delete;

    // Marches the cloud shell into 'target' (RGBA16F, usually half the scene
    // resolution) as premultiplied radiance in rgb and transmittance in alpha.
    // 'depth' is the full-resolution scene depth that bounds each ray. The
    // camera, sun and irradiance reuse the atmosphere view (body-fixed frame).
    // Only the first cloud layer is drawn.
    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& depth,
        celestial_atmosphere::GpuAtmosphereLuts& luts,
        GpuCloudFieldProduct& field,
        rhi::Texture& target,
        u32 width,
        u32 height,
        f64 referenceRadiusMeters,
        const CloudLayerParameters& layer,
        const celestial_atmosphere::AtmosphereParameters& atmosphere,
        const celestial_atmosphere::AtmosphereRenderView& view);

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
        const celestial_atmosphere::AtmosphereRenderView& view);

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
    std::unique_ptr<rhi::GraphicsPipeline> shadowPipeline_;
    // Baked 32^3 tileable cellular noise (base shape + erosion octaves).
    std::unique_ptr<rhi::Buffer> noise_;
};
} // namespace orbit::celestial_clouds
