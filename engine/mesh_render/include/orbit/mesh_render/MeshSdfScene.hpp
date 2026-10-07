#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/mesh_render/MeshSurface.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <orbit/lighting/LightingView.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace orbit::mesh_render
{
// The merged unsigned distance field of every placed mesh that has
// one, as storage buffers in a camera-independent volume (frame coordinates).
// Used for software ray tracing (sphere tracing) of the meshes: GI rays, and
// the SDF debug view. The volume only changes when the set of meshes, their
// placement or a model's generation changes.
struct SdfSceneVolume
{
    bool ready{false};
    // Frame coordinates of voxel (0, 0, 0)'s centre.
    math::Double3 originInFrameMeters{};
    f32 voxelSize{0.25F};
    std::array<u32, 3> dimensions{};
    // Changes whenever the contents change (consumers invalidate caches).
    u64 revision{0U};
    rhi::Buffer* distance{nullptr};
    rhi::Buffer* albedo{nullptr};
    rhi::Buffer* normal{nullptr};   // world-space octahedral
    rhi::Buffer* emissive{nullptr}; // RGB8 * emissiveScale
    // Outgoing diffuse radiance of each surface voxel (RGBA32F, scene units),
    // refreshed by MeshSdfScene::Light(); read by GI rays at their hits.
    rhi::Buffer* radiance{nullptr};
    f32 emissiveScale{1.0F};
};

// Per-frame lighting inputs of the surface voxels, in the same frame and
// units as the direct lighting pass.
struct SdfLightingInput
{
    math::Float3 toSun{0.0F, 1.0F, 0.0F};
    // Sun irradiance as a fraction of the solar constant (1 at 1 AU).
    f32 sunIrradiance{0.0F};
    // Sky irradiance on an up-facing surface, same units; zero disables it.
    math::Float3 skyIrradiance{};
    // Local up (radial) used to tell sky from ground for escaping rays.
    math::Float3 up{0.0F, 1.0F, 0.0F};
    u32 frame{0U};
};

// A Visibility Proxy (sphere or oriented box) as the field sees it, in frame
// coordinates, so non-mesh structures also occlude and bounce light.
struct SdfProxyPrimitive
{
    math::Double3 center{};
    math::Float3 axisX{1.0F, 0.0F, 0.0F};
    math::Float3 axisY{0.0F, 1.0F, 0.0F};
    math::Float3 axisZ{0.0F, 0.0F, 1.0F};
    // Box half extents, or the radius in x for a sphere.
    math::Float3 halfExtents{0.5F, 0.5F, 0.5F};
    bool box{true};
};

// Terrain height above the tangent plane at `center`, on a regular grid in
// the tangent plane (`east`, `north`, `up` are frame-space unit vectors).
// Cell (i, j) is at east = (i - (count - 1) / 2) * cell, likewise north.
struct SdfTerrainPatch
{
    math::Double3 center{};
    math::Float3 east{1.0F, 0.0F, 0.0F};
    math::Float3 north{0.0F, 0.0F, 1.0F};
    math::Float3 up{0.0F, 1.0F, 0.0F};
    f32 cellMeters{1.0F};
    u32 count{0U};
    math::Float3 albedo{0.30F, 0.27F, 0.22F};
    // Changes whenever the heights change.
    u64 revision{0U};
    std::vector<f32> heights;
};

// Non-mesh geometry stamped into the global field around the meshes.
struct SdfExtraGeometry
{
    std::vector<SdfProxyPrimitive> primitives;
    std::shared_ptr<const SdfTerrainPatch> terrain;
};

class MeshSdfScene
{
public:
    MeshSdfScene(
        rhi::Device& device,
        const shader::Compiler& compiler);

    // Rebuilds the global field when the instance set changed since the
    // previous call. Must be called outside a render pass. `instances` are
    // camera-relative as drawn; `cameraInFrameMeters` makes them frame-fixed.
    void Update(
        rhi::CommandList& commands,
        std::span<const MeshInstance> instances,
        const math::Double3& cameraInFrameMeters,
        const SdfExtraGeometry* extra = nullptr);

    // Refreshes a quarter of the surface voxels' radiance: sun (soft shadow
    // traced through the field), sky and a multi-bounce term gathered from the
    // previous radiance. Outside a render pass, after Update().
    void Light(rhi::CommandList& commands, const SdfLightingInput& input);

    [[nodiscard]] const SdfSceneVolume& Volume() const noexcept
    {
        return volume_;
    }

private:
    struct Retired
    {
        std::vector<std::unique_ptr<rhi::Buffer>> buffers;
        u64 retireAtTick{0U};
    };

    rhi::Device& device_;
    std::unique_ptr<rhi::ComputePipeline> clearPipeline_;
    std::unique_ptr<rhi::ComputePipeline> mergePipeline_;
    std::unique_ptr<rhi::ComputePipeline> lightPipeline_;
    std::unique_ptr<rhi::ComputePipeline> primitivePipeline_;
    std::unique_ptr<rhi::ComputePipeline> terrainPipeline_;
    std::unique_ptr<rhi::Buffer> radiance_;
    std::vector<std::unique_ptr<rhi::Buffer>> lightParameters_;
    std::vector<u64> lightParameterTicks_;

    SdfSceneVolume volume_;
    std::unique_ptr<rhi::Buffer> distance_;
    std::unique_ptr<rhi::Buffer> albedo_;
    std::unique_ptr<rhi::Buffer> normal_;
    std::unique_ptr<rhi::Buffer> emissive_;
    std::size_t allocatedVoxels_{0U};
    u64 signature_{0U};
    u64 revision_{0U};
    u64 tick_{0U};
    std::vector<Retired> retired_;
};

inline constexpr u32 kSdfDebugModes = 6U;

// Debug view: sphere traces the global field from the camera and shades what
// it finds, to check the field against the rasterised image.
//   1 = SDF shaded (albedo and sun term)  2 = step count heat map
//   3 = distance to first hit             4 = left half SDF, right half scene
//   5 = surface radiance (sun + sky + multi-bounce) as stored
class MeshSdfDebugRenderer
{
public:
    MeshSdfDebugRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    // `destination` (RGBA16F, UnorderedAccess) receives the result; `color`
    // (ShaderResource) is the scene colour it falls back to.
    void Draw(
        rhi::CommandList& commands,
        const SdfSceneVolume& volume,
        rhi::Texture& color,
        rhi::Texture& destination,
        u32 width,
        u32 height,
        const lighting::LightingView& view,
        const math::Float3& directionToSun,
        u32 mode);

private:
    rhi::Device& device_;
    std::unique_ptr<rhi::ComputePipeline> pipeline_;
};
} // namespace orbit::mesh_render
