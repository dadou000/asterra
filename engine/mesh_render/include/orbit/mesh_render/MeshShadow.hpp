#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/lighting/LightingView.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/mesh_render/MeshSurface.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace orbit::mesh_render
{
inline constexpr u32 kMeshShadowMapSize = 2048U;

// Sky occlusion maps: one tile per sky direction in a 4 x 4 atlas. Direction
// 0 is straight up; 1..5 sit 72 degrees above the horizon, 72 degrees apart;
// 6..15 sit 36 degrees above it, 36 degrees apart (offset half a step). Many
// directions keep any one direction's hard shadow edge from showing.
inline constexpr u32 kMeshSkyDirections = 16U;
inline constexpr u32 kMeshSkyTileSize = 256U;
inline constexpr u32 kMeshSkyAtlasWidth = 4U * kMeshSkyTileSize;
inline constexpr u32 kMeshSkyAtlasHeight = 4U * kMeshSkyTileSize;

// Orthographic sun view that encloses every mesh instance. All vectors are
// camera-relative scene-frame metres. The window is the bounding sphere of
// the casters projected along the sun direction: a point can only be shadowed
// by a caster if its projection falls inside that disc, so receivers anywhere
// (terrain, proxies, the room) are handled by one small map.
struct MeshShadowFrame
{
    std::array<f32, 3> center{};
    f32 radius{1.0F};
    std::array<f32, 3> right{1.0F, 0.0F, 0.0F};
    std::array<f32, 3> up{0.0F, 1.0F, 0.0F};
    // Unit vector toward the sun.
    std::array<f32, 3> toSun{0.0F, 0.0F, 1.0F};
    u32 mapSize{kMeshShadowMapSize};
    // Tangent of the sun's angular radius as seen from the receiver; sets the
    // penumbra width (PCSS). 0.00465 is the real Sun from 1 AU.
    f32 sunTanHalfAngle{0.00465F};
};

// Fits the sun window around the resident instances; nullopt when none is
// resident or the sun direction is degenerate. The window centre is snapped to
// whole texels in a camera-independent frame so shadow edges do not shimmer
// when the camera moves.
[[nodiscard]] std::optional<MeshShadowFrame> BuildMeshShadowFrame(
    std::span<const MeshInstance> instances,
    const math::Float3& directionToSun,
    const math::Double3& cameraPositionInFrameMeters,
    u32 mapSize = kMeshShadowMapSize);

// One sky direction's view: the unit vector toward that part of the sky and
// an orthonormal window basis. The resolve shader derives the same triple
// from `localUp` with identical maths, so both must change together.
struct MeshSkyView
{
    std::array<f32, 3> toSky{};
    std::array<f32, 3> right{};
    std::array<f32, 3> up{};
};

[[nodiscard]] MeshSkyView MeshSkyDirection(
    u32 index,
    const std::array<f32, 3>& localUp);

// Unit radial direction at the shadow frame's centre (the local "up" used to
// tell sky from ground), from the camera position in the frame; (0,1,0) if the
// centre is at the body origin.
[[nodiscard]] std::array<f32, 3> MeshLocalUp(
    const MeshShadowFrame& frame,
    const math::Double3& cameraPositionInFrameMeters);

// Renders the caster depth (distance from the sun side of the window, 0..1)
// into an R32_Float colour map, alpha-testing cut-out materials.
class MeshShadowMapRenderer
{
public:
    MeshShadowMapRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    // `colorMap` (R32_Float) and `depthMap` (D32_Float) must be mapSize
    // square. Clears both and draws every part of every instance.
    void Draw(
        rhi::CommandList& commands,
        const MeshLibrary& library,
        std::span<const MeshInstance> instances,
        const MeshShadowFrame& frame,
        rhi::Texture& colorMap,
        rhi::Texture& depthMap);

    // Same, for the kMeshSkyDirections sky views into a
    // kMeshSkyAtlasWidth x kMeshSkyAtlasHeight atlas (window = `frame`'s
    // bounding sphere, so the sun-window snapping is harmlessly reused).
    void DrawSky(
        rhi::CommandList& commands,
        const MeshLibrary& library,
        std::span<const MeshInstance> instances,
        const MeshShadowFrame& frame,
        const std::array<f32, 3>& localUp,
        rhi::Texture& colorAtlas,
        rhi::Texture& depthAtlas);

private:
    struct View
    {
        std::array<f32, 3> center{};
        f32 radius{1.0F};
        std::array<f32, 3> right{};
        std::array<f32, 3> up{};
        std::array<f32, 3> toSun{};
        u32 originX{0U};
        u32 originY{0U};
        u32 size{0U};
    };

    void DrawViews(
        rhi::CommandList& commands,
        const MeshLibrary& library,
        std::span<const MeshInstance> instances,
        std::span<const View> views,
        rhi::Texture& colorMap,
        rhi::Texture& depthMap,
        u32 width,
        u32 height);

    struct RetiredRecords
    {
        std::unique_ptr<rhi::Buffer> buffer;
        u64 retireAtTick{0U};
    };

    rhi::Device& device_;
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
    std::vector<RetiredRecords> records_;
    u64 tick_{0U};
};

// Inputs of the sky fill written for mesh pixels.
struct MeshSkyFill
{
    // Atmosphere sky irradiance on an up-facing surface (scene units);
    // zero disables the fill.
    math::Float3 irradiance{};
    std::array<f32, 3> localUp{0.0F, 1.0F, 0.0F};
};

// Folds the mesh shadow map into the sun-visibility texture direct lighting
// reads (R channel) and replaces the sky fill (GBA) of mesh pixels with one
// occluded by the meshes themselves. With `initialize` the texture is written
// fresh (no proxy pass ran); otherwise visibility is min()ed with what the
// proxy pass wrote and only mesh pixels' sky fill is overwritten.
class MeshSunShadowRenderer
{
public:
    MeshSunShadowRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    // `target` in UnorderedAccess; the sampled inputs in ShaderResource
    // (`depth` in DepthRead).
    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& target,
        rhi::Texture& surfaceNormalMetallic,
        rhi::Texture& surfaceEmissionClass,
        rhi::Texture& depth,
        rhi::Texture& shadowMap,
        rhi::Texture& skyAtlas,
        u32 width,
        u32 height,
        const lighting::LightingView& view,
        const MeshShadowFrame& frame,
        const MeshSkyFill& sky,
        bool initialize);

private:
    rhi::Device& device_;
    std::unique_ptr<rhi::ComputePipeline> pipeline_;
    struct RetiredBuffer
    {
        std::unique_ptr<rhi::Buffer> buffer;
        u64 retireAtTick{0U};
    };
    std::vector<RetiredBuffer> parameters_;
    u64 tick_{0U};
};
} // namespace orbit::mesh_render
