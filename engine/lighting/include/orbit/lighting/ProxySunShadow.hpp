#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/lighting/HardwareRayQueryVisibility.hpp>
#include <orbit/lighting/LightingView.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <array>
#include <memory>

namespace orbit::lighting
{
struct ProxySunShadowSettings
{
    // Farthest distance a sun ray is traced toward the star. Proxies are
    // authored scene geometry, so a few kilometres covers any structure.
    f32 maximumDistanceMeters{4000.0F};
    // Constant surface offset along the normal; a view-distance term is added
    // in the shader so far pixels do not self-shadow on depth quantisation.
    f32 normalBiasMeters{0.02F};
    // Sky irradiance on an up-facing surface, in the units of the sun's
    // irradiance scale (the atmosphere's sky summary). Zero disables the sky
    // fill; it is added to authored proxy surfaces only.
    math::Float3 skyIrradiance{};
    // Cosine-weighted hemisphere rays per proxy-surface pixel, capped at 64.
    u32 skyRayCount{12U};
    // How far a sky ray looks for another proxy.
    f32 skyMaximumDistanceMeters{150.0F};
};

inline constexpr u32 kProxySunShadowConstantDwords = 32U;

// Packs the compute push constants. Exposed so the layout the shader reads is
// covered by a CPU test; the camera-to-scene offset is the camera position in
// the proxy scene's frame minus the scene's GPU origin (the acceleration
// structure is stored relative to that origin). The local up direction used to
// tell sky from ground is the camera's radial direction in the body frame:
// proxies span hundreds of metres on a body of thousands of kilometres, so one
// direction serves the whole scene.
[[nodiscard]] std::array<u32, kProxySunShadowConstantDwords>
PackProxySunShadowConstants(
    const LightingView& view,
    u32 width,
    u32 height,
    u32 primitiveCount,
    const math::Float3& directionToLight,
    const math::Double3& cameraToSceneOriginMeters,
    const ProxySunShadowSettings& settings);

// Lighting from authored Visibility Proxies, hardware ray queries per visible
// pixel. Writes a full-resolution RGBA texture: R = sun visibility (1 lit, 0
// occluded), GBA = sky irradiance already scaled by the fraction of the sky
// hemisphere the other proxies leave open (non-zero on proxy surfaces only).
// DirectLightingRenderer multiplies R into the direct stellar term, as it does
// the cloud shadow, and adds GBA as albedo / pi fill. Terrain and the sky are
// not proxies, so only authored structures cast.
class ProxySunShadowRenderer
{
public:
    ProxySunShadowRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    // True when the device can ray query and the pipeline was created.
    [[nodiscard]] bool Supported() const noexcept;

    // `scene` must be Ready() and `target` in ResourceState::UnorderedAccess;
    // `normalMetallic`, `emissionClass` and `depth` in ShaderResource/DepthRead
    // as for the other lighting compute passes.
    void Draw(
        rhi::CommandList& commands,
        HardwareRayQueryVisibilityBatch& scene,
        rhi::Texture& target,
        rhi::Texture& surfaceNormalMetallic,
        rhi::Texture& surfaceEmissionClass,
        rhi::Texture& depth,
        u32 width,
        u32 height,
        const LightingView& view,
        const math::Float3& directionToLight,
        const ProxySunShadowSettings& settings = {});

private:
    std::unique_ptr<rhi::ComputePipeline> pipeline_;
};
} // namespace orbit::lighting
