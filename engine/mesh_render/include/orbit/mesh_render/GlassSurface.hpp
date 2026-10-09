#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/lighting/LightingView.hpp>
#include <orbit/lighting/ReflectionScene.hpp>
#include <orbit/lighting/ScreenSpaceFinalGather.hpp>
#include <orbit/mesh_render/MeshLibrary.hpp>
#include <orbit/mesh_render/MeshShadow.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace orbit::mesh_render
{
// Runtime glass shader, exposed for headless compile validation.
[[nodiscard]] std::string BuildGlassPixelShaderSource();

// Shape ids match world_model::PrimitiveShape.
enum class GlassShape : u32
{
    Box = 0U,
    Sphere = 1U,
    Cylinder = 2U,
    Capsule = 3U,
    Plane = 4U
};

// Thickness given to a Plane so it behaves as a glass pane.
inline constexpr f32 kGlassPaneHalfThicknessMeters = 0.01F;

// One placed glass body for a frame. `rows` has the MeshInstance meaning
// (local to camera-relative scene-frame metres, row-major 3x4, rotation and
// translation only: the shape's size lives in `halfExtents`). The optics are
// evaluated analytically from the shape, so no mesh is rasterised for
// refraction; `model` (generated geometry of the same shape) only serves as a
// sun-shadow caster and may be null while it is still loading.
struct GlassInstance
{
    const MeshModel* model{nullptr};
    std::array<f32, 12> rows{};
    GlassShape shape{GlassShape::Sphere};
    std::array<f32, 3> halfExtents{0.5F, 0.5F, 0.5F};
    // Transmittance per metre of path inside the glass: white is clear glass,
    // saturated colours absorb the other channels.
    std::array<f32, 3> tint{1.0F, 1.0F, 1.0F};
    f32 indexOfRefraction{1.5F};
    // Whether sunlight focused through this body is splatted onto receivers.
    bool caustics{true};
    bool castShadows{true};
};

// Lighting the glass passes need, in the scene frame the instances use.
struct GlassLighting
{
    // Unit vector toward the sun.
    std::array<f32, 3> toSun{0.0F, 1.0F, 0.0F};
    // Irradiance on a surface facing the sun (colour x scale, scene units);
    // zero disables caustics.
    std::array<f32, 3> sunIrradiance{};
    // Sky radiance: what a ray sees when it leaves the screen and the mesh
    // distance field finds nothing either, travelling upward.
    std::array<f32, 3> environment{};
    // Unit vector pointing away from the body's centre at the camera; tells
    // sky (above) from ground (below) for those unresolved rays.
    std::array<f32, 3> localUp{0.0F, 1.0F, 0.0F};
    // 0 = the sun is hidden behind the local horizon or otherwise not
    // reaching the glass; scales caustic energy.
    f32 sunVisibility{1.0F};
};

// Camera basis and projection terms shared by the shaders; kept CPU-side so
// the layout the shader reads is tested (see GlassSurfaceTests).
struct GlassCamera
{
    std::array<f32, 3> right{};
    std::array<f32, 3> up{};
    std::array<f32, 3> forward{};
    // tan(half vertical fov) * aspect and tan(half vertical fov).
    f32 tanHalfX{1.0F};
    f32 tanHalfY{1.0F};
    f32 nearPlane{0.05F};
    f32 farPlane{1000.0F};
};

[[nodiscard]] GlassCamera MakeGlassCamera(
    const lighting::LightingView& view,
    u32 width,
    u32 height) noexcept;

// Pixel rectangle (left, top, right, bottom, clamped to the target) that
// contains the projected oriented bounding box of `instance`; the full target
// when any corner is at or behind the near plane. nullopt when the box is
// entirely off screen.
struct GlassScreenRect
{
    i32 left{0};
    i32 top{0};
    i32 right{0};
    i32 bottom{0};
};
[[nodiscard]] std::optional<GlassScreenRect> GlassScreenBounds(
    const GlassInstance& instance,
    const GlassCamera& camera,
    u32 width,
    u32 height) noexcept;

// Floats per instance in the structured buffer the shaders read. After the
// instances come three float4s (kGlassTailFloats): for the caustic pass the
// sun shadow window (centre + radius, right + map size, up + enabled flag), for
// the glass pass the distance field (origin + voxel size, dimensions + enabled
// flag, local up).
inline constexpr u32 kGlassTailFloats = 12U;
inline constexpr u32 kGlassRecordFloats = 24U;
[[nodiscard]] std::array<f32, kGlassRecordFloats> PackGlassRecord(
    const GlassInstance& instance) noexcept;

// Photons per side of the caustic emission grid for a body of the given
// bounding radius; more for larger bodies, bounded for cost.
[[nodiscard]] u32 GlassPhotonGrid(f32 boundingRadiusMeters) noexcept;

// Refractive glass (analytic shapes, two-sided refraction with total internal
// reflection, Fresnel reflection, Beer-Lambert absorption) drawn over the lit
// scene, plus photon-splatted sun caustics on the receivers behind it. Both
// work from the surface buffer's depth and the already lit scene colour, so
// they run after lighting and the atmosphere and write only scene colour.
class GlassSurfaceRenderer
{
public:
    GlassSurfaceRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void BeginFrame(u32 completedSlot, u32 framesInFlight);

    // Copies the lit scene into `backdrop` (same size and format) so the glass
    // pass can read what lies behind while writing the scene. `sceneColor`
    // in ShaderResource, `backdrop` in RenderTarget.
    void CopyBackdrop(
        rhi::CommandList& commands,
        rhi::Texture& sceneColor,
        rhi::Texture& backdrop,
        u32 width,
        u32 height);

    // Adds sunlight refracted through every caustic-casting instance onto the
    // surfaces it lands on. `sceneColor` in RenderTarget; the rest sampled
    // (`depth` in DepthRead). `sunShadowMap` (R32_Float, ShaderResource) and
    // `shadowFrame` are the mesh sun shadow map the lighting already built:
    // a photon whose emission point is shadowed there carries no light. Without
    // them every photon is assumed to reach the glass.
    void DrawCaustics(
        rhi::CommandList& commands,
        std::span<const GlassInstance> instances,
        rhi::Texture& sceneColor,
        rhi::Texture& depth,
        rhi::Texture& surfaceBaseRoughness,
        rhi::Texture& surfaceNormalMetallic,
        rhi::Texture& surfaceEmissionClass,
        u32 width,
        u32 height,
        const lighting::LightingView& view,
        const GlassLighting& lighting,
        rhi::Texture* sunShadowMap = nullptr,
        const MeshShadowFrame* shadowFrame = nullptr);

    // Replaces the scene colour under each instance with the refracted
    // backdrop plus the Fresnel reflection. `sceneColor` in RenderTarget;
    // `backdrop` in ShaderResource, `depth` in DepthRead. Rays that leave the
    // screen are traced through the mesh distance field `sdf` (null: they see
    // the environment colour).
    void DrawGlass(
        rhi::CommandList& commands,
        std::span<const GlassInstance> instances,
        rhi::Texture& sceneColor,
        rhi::Texture& backdrop,
        rhi::Texture& depth,
        u32 width,
        u32 height,
        const lighting::LightingView& view,
        const GlassLighting& lighting,
        const lighting::SdfGatherInput* sdf = nullptr,
        const lighting::ReflectionSceneInput* exact = nullptr);

private:
    struct RetiredRecords
    {
        std::unique_ptr<rhi::Buffer> buffer;
        u64 pendingFrames{0U};
    };

    // Instance records followed by `tail` (three float4s the shader reads
    // behind the last instance).
    [[nodiscard]] rhi::Buffer& UploadRecords(
        std::span<const GlassInstance> instances,
        std::span<const f32> tail);

    rhi::Device& device_;
    std::unique_ptr<rhi::GraphicsPipeline> copyPipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> causticPipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> glassPipeline_;
    std::vector<RetiredRecords> records_;
    std::unique_ptr<rhi::Buffer> dummySdf_;
    u64 pendingFramesMask_{~0ULL};
};
} // namespace orbit::mesh_render
