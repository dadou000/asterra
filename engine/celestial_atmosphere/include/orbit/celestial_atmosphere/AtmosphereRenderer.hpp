#pragma once

#include <orbit/celestial_atmosphere/Atmosphere.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::celestial_atmosphere
{
// Observer and illumination for one atmosphere composite, all expressed in
// the body-fixed frame the atmosphere parameters are defined in.
struct AtmosphereRenderView
{
    math::Double3 cameraPositionMeters{};
    math::Float3 forward{0.0F, 0.0F, 1.0F};
    math::Float3 up{0.0F, 1.0F, 0.0F};
    f32 verticalFovRadians{1.0F};
    f32 nearPlaneMeters{0.05F};
    f32 farPlaneMeters{1.0e7F};

    // Unit direction towards the illuminating star.
    math::Float3 sunDirection{0.0F, 0.0F, 1.0F};

    // Stellar irradiance at the body divided by the scene reference
    // irradiance, so the output is in the same scene-linear radiance units
    // as the shared direct-lighting pass.
    f32 irradianceScale{1.0F};
};

// Per-pixel single + multiple scattering composite through the physical
// atmosphere shell: sceneColor * T + L, where T and L are integrated along
// the view ray to the first surface (depth buffer or bottom sphere) or out
// of the top of the atmosphere. Sun visibility uses the static transmittance
// LUT and multiple scattering the M21 multi-scattering response LUT, so an
// orbital observer resolves the thin limb that a sky-view LUT cannot.
class AtmosphereRenderer
{
public:
    AtmosphereRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);
    ~AtmosphereRenderer();

    AtmosphereRenderer(const AtmosphereRenderer&) = delete;
    AtmosphereRenderer& operator=(const AtmosphereRenderer&) = delete;

    // sceneColor and depth are read (ShaderResource / DepthRead); the
    // composited result is written to target (RGBA16F render target).
    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& sceneColor,
        rhi::Texture& depth,
        GpuAtmosphereLuts& luts,
        rhi::Texture& target,
        u32 width,
        u32 height,
        const AtmosphereParameters& parameters,
        const AtmosphereRenderView& view);

private:
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
};
} // namespace orbit::celestial_atmosphere
