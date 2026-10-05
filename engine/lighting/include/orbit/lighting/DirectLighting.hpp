#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/lighting/LightingView.hpp>
#include <orbit/lighting/LocalLightRegistry.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::lighting
{
struct DirectionalLight
{
    // Direction from the shaded surface toward the light, expressed in the
    // same body/view orientation as SurfaceNormalMetallic.
    math::Float3 directionToLight{0.0F, 1.0F, 0.0F};
    math::Float3 colorLinear{1.0F, 1.0F, 1.0F};
    f32 irradianceScale{1.0F};
};

struct DirectLightingSettings
{
    // Small physically-motivated sky/ground floor until M21 supplies the
    // atmosphere/sky-visibility term. It is deliberately a shared lighting
    // setting rather than renderer-local preview fill.
    f32 ambientIrradianceScale{0.035F};
};

class DirectLightingRenderer
{
public:
    DirectLightingRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& surfaceBaseRoughness,
        rhi::Texture& surfaceNormalMetallic,
        rhi::Texture& surfaceEmissionClass,
        rhi::Texture& depth,
        rhi::Buffer& localLights,
        rhi::Buffer& tileOffsets,
        rhi::Buffer& tileLightIndices,
        rhi::Texture& targetSceneColor,
        u32 width,
        u32 height,
        const LightingView& view,
        const DirectionalLight& light,
        const TiledLightGrid& localLightGrid,
        // Optional previous-completed M38 GPU particle light grid. When
        // present it is the shared authority for smoke transmittance and
        // low-frequency particle emission; no particle-state readback occurs.
        rhi::Buffer* particleLightGrid = nullptr,
        // Optional reduced-resolution sun transmittance from the cloud layer
        // (red channel, 1 = lit); multiplies the direct stellar term.
        rhi::Texture* cloudShadow = nullptr,
        // Optional full-resolution lighting from authored Visibility Proxies:
        // red = sun visibility (1 = lit) multiplies the direct stellar term;
        // green-blue-alpha = sky irradiance weighted by the open hemisphere,
        // added as albedo / pi fill (non-zero on proxy surfaces only).
        rhi::Texture* proxySunShadow = nullptr,
        // Optional radiance cache (cells and levels, as built for the cache
        // sampler): the sky-only channel is added as albedo / pi fill at
        // `skyCacheStrength` (1 = physical). Null or zero turns it off.
        rhi::Buffer* radianceCells = nullptr,
        rhi::Buffer* radianceLevels = nullptr,
        u32 radianceLevelCount = 0U,
        f32 skyCacheStrength = 1.0F,
        const DirectLightingSettings& settings = {});

private:
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
    std::unique_ptr<rhi::Buffer> dummyParticleLightGrid_;
};
} // namespace orbit::lighting