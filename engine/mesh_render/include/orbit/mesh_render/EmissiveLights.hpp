#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/lighting/LightingView.hpp>
#include <orbit/lighting/ScreenSpaceFinalGather.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace orbit::mesh_render
{
// Most emitters the pass lights from; extra ones are dropped (the dimmest
// first, see SelectEmissiveLights).
inline constexpr u32 kMaxEmissiveLights = 16U;

// One emissive body as a Lambertian sphere light. Positions are camera-relative
// scene-frame metres, like mesh instances.
struct EmissiveLight
{
    std::array<f32, 3> position{};
    // Radius of the sphere with the same surface area as the body, so its far
    // field irradiance matches the real emitter's.
    f32 radius{0.5F};
    // Radius of a sphere enclosing the body: shadow rays stop short of it so
    // the emitter does not occlude itself.
    f32 boundingRadius{0.5F};
    // Emitted radiance, scene-linear (1 unit = kSceneLuminanceNitsPerUnit).
    std::array<f32, 3> radiance{1.0F, 1.0F, 1.0F};
};

// Surface area (m^2) of a primitive of the given shape id (0 Box, 1 Sphere,
// 2 Cylinder, 3 Capsule, 4 Plane; a plane emits from both sides) and half
// extents; zero for unknown shapes or non-positive extents.
[[nodiscard]] f32 EmissiveSurfaceArea(
    u32 shape,
    const std::array<f32, 3>& halfExtents) noexcept;

// Radius of a sphere with the given surface area.
[[nodiscard]] f32 EquivalentSphereRadius(f32 surfaceArea) noexcept;

// Keeps at most kMaxEmissiveLights lights, preferring the ones that matter most
// at the camera (highest radiant power over squared distance). Lights that emit
// nothing are dropped.
[[nodiscard]] std::array<EmissiveLight, kMaxEmissiveLights> SelectEmissiveLights(
    std::span<const EmissiveLight> lights,
    u32& count);

// Direct light from emissive meshes: an analytic sphere-light irradiance per
// pixel with a soft shadow traced through the mesh distance field, so a lamp
// lights its surroundings smoothly and is occluded by geometry. It replaces
// the noisy emission the gather would otherwise pick up by chance from small
// bright voxels (primitive emitters are therefore left out of the field's
// emissive channel).
class EmissiveLightRenderer
{
public:
    EmissiveLightRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    // Writes the added lighting (RGBA16F) into `target`, which must be in
    // UnorderedAccess; the sampled inputs in ShaderResource (`depth` in
    // DepthRead). `sdf` may be null: lights then cast no shadows.
    void Light(
        rhi::CommandList& commands,
        std::span<const EmissiveLight> lights,
        rhi::Texture& target,
        rhi::Texture& surfaceBaseRoughness,
        rhi::Texture& surfaceNormalMetallic,
        rhi::Texture& surfaceEmissionClass,
        rhi::Texture& depth,
        u32 width,
        u32 height,
        const lighting::LightingView& view,
        const lighting::SdfGatherInput* sdf);

    // Adds `lighting` onto `sceneColor` (RenderTarget); `lighting` in
    // ShaderResource.
    void Composite(
        rhi::CommandList& commands,
        rhi::Texture& lighting,
        rhi::Texture& sceneColor,
        u32 width,
        u32 height);

private:
    struct RetiredBuffer
    {
        std::unique_ptr<rhi::Buffer> buffer;
        u64 retireAtTick{0U};
    };

    rhi::Device& device_;
    std::unique_ptr<rhi::ComputePipeline> lightPipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> compositePipeline_;
    std::unique_ptr<rhi::Buffer> dummySdf_;
    std::vector<RetiredBuffer> lightBuffers_;
    u64 tick_{0U};
};
} // namespace orbit::mesh_render
