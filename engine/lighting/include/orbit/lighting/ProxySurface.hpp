#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/lighting/LightingView.hpp>
#include <orbit/lighting/SoftwareProxyVisibility.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <array>
#include <memory>

namespace orbit::lighting
{
inline constexpr u32 kProxySurfaceConstantDwords = 24U;

struct ProxySurfaceSettings
{
    // Albedo of proxies with Material ID 0; other IDs get a stable tint so
    // separate structures read apart.
    math::Float3 defaultAlbedo{0.55F, 0.53F, 0.50F};
    f32 roughness{0.85F};
};

// Scene-frame primitives of one SoftwareProxyScene, uploaded for rasterising.
// Independent of ray-query support: visible proxies only need the buffer.
class ProxySurfaceGeometry
{
public:
    void Rebuild(
        rhi::Device& device,
        const SoftwareProxyScene& scene,
        math::Double3 gpuOriginInFrameMeters);

    [[nodiscard]] bool Ready() const noexcept;
    [[nodiscard]] u32 PrimitiveCount() const noexcept;
    [[nodiscard]] rhi::Buffer* PrimitiveBuffer() const noexcept;
    [[nodiscard]] math::Double3 GpuOriginInFrameMeters() const noexcept;

    // Where the proxies are, in the scene frame: the mean of their centres and
    // a radius that bounds all of them. Lets the renderer decide whether the
    // GPU origin needs refreshing without touching the buffers.
    [[nodiscard]] math::Double3 CentroidInFrameMeters() const noexcept;
    [[nodiscard]] f64 BoundingRadiusMeters() const noexcept;

private:
    std::unique_ptr<rhi::Buffer> buffer_;
    u32 count_{0U};
    math::Double3 origin_{};
    math::Double3 centroid_{};
    f64 radius_{0.0};
};

// The GPU scenes (acceleration structure, primitive buffers) store positions
// as float32 relative to the GPU origin they were built at, so they are only
// exact while the camera stays near that origin: at a few million metres a
// float resolves about a metre, and surfaces reconstructed from the exact
// depth buffer then land inside geometry that was snapped to that grid.
// True when the origin has drifted past `driftThresholdMeters` from the camera
// while the camera is within `relevanceMeters` of the proxies (far away they
// are sub-pixel and not worth rebuilding for).
[[nodiscard]] bool ProxyGpuOriginIsStale(
    const math::Double3& cameraInFrameMeters,
    const math::Double3& gpuOriginInFrameMeters,
    const math::Double3& proxyCentroidInFrameMeters,
    f64 proxyBoundingRadiusMeters,
    f64 driftThresholdMeters = 1'500.0,
    f64 relevanceMeters = 20'000.0) noexcept;

// Packs the push constants: the camera basis (RH, as the lighting passes
// reconstruct positions), projection terms, scene-to-camera offset and the
// default material. Exposed so the layout the shader reads is CPU-tested.
[[nodiscard]] std::array<u32, kProxySurfaceConstantDwords>
PackProxySurfaceConstants(
    const LightingView& view,
    u32 width,
    u32 height,
    const math::Double3& sceneToCameraMeters,
    const ProxySurfaceSettings& settings);

// Rasterises authored Visibility Proxies as lit, depth-tested geometry into
// the deferred surface buffer (SceneColor, BaseRoughness, NormalMetallic,
// EmissionClass, reverse-Z depth), after the terrain pass. Proxies are then
// ordinary RigidGeometry surfaces: direct sun (and the proxy sun shadow),
// the final gather and the radiance cache all light them. Boxes use
// rasterised depth; spheres are ray-traced in the pixel shader.
class ProxySurfaceRenderer
{
public:
    ProxySurfaceRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    // The caller has bound nothing: this sets the four colour targets and
    // depth itself and preserves their contents.
    void Draw(
        rhi::CommandList& commands,
        ProxySurfaceGeometry& geometry,
        rhi::Texture& sceneColor,
        rhi::Texture& surfaceBaseRoughness,
        rhi::Texture& surfaceNormalMetallic,
        rhi::Texture& surfaceEmissionClass,
        rhi::Texture& depth,
        u32 width,
        u32 height,
        const LightingView& view,
        const ProxySurfaceSettings& settings = {});

private:
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
};
} // namespace orbit::lighting
