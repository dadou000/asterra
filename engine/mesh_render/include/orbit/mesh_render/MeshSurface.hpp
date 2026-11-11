#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/lighting/LightingView.hpp>
#include <orbit/math/RigidTransform.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/mesh_render/MeshLibrary.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace orbit::mesh_render
{
class MeshSdfScene;
struct SdfExtraGeometry;

// One placed copy of a model for a frame. The transform maps model-local
// points to camera-relative scene-frame metres (the frame the lighting
// passes reconstruct positions in), computed in double precision by the
// caller so large body-centred coordinates never reach float32.
struct MeshInstance
{
    const MeshModel* model{nullptr};
    // Row-major 3x4: x' = row0 . (x, y, z, 1).
    std::array<f32, 12> rows{};
};

// Builds a MeshInstance transform: translation is the instance origin minus
// the camera position, rotation columns are the scene-frame image of the
// model's X/Y/Z axes, uniformScale scales the model.
[[nodiscard]] std::array<f32, 12> MakeInstanceRows(
    const math::Double3x3& rotation,
    f64 uniformScale,
    const math::Double3& originRelativeToCameraMeters) noexcept;

inline constexpr u32 kMeshSurfacePushDwords = 16U;
// float4s per draw record in the structured buffer the shaders read.
inline constexpr u32 kMeshDrawRecordVectors = 6U;

// Rasterises imported meshes into the deferred surface buffer after the
// terrain/proxy passes, as ordinary rigid surfaces (class RigidGeometry,
// representation LocalMesh): direct sun, the final gather and the radiance
// cache light them like any other near-field surface.
class MeshSurfaceRenderer
{
public:
    MeshSurfaceRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    // Optional: the global distance field to keep up to date. Updated right
    // after the library pump, outside the render pass.
    void SetSdfScene(MeshSdfScene* scene) noexcept
    {
        sdfScene_ = scene;
    }

    // Pumps `library` (uploads, hot reload) and draws every instance whose
    // model is resident. The caller has bound nothing: this sets the four
    // colour targets and depth itself and preserves their contents.
    void Draw(
        rhi::CommandList& commands,
        MeshLibrary& library,
        std::span<const MeshInstance> instances,
        rhi::Texture& sceneColor,
        rhi::Texture& surfaceBaseRoughness,
        rhi::Texture& surfaceNormalMetallic,
        rhi::Texture& surfaceEmissionClass,
        rhi::Texture& depth,
        u32 width,
        u32 height,
        const lighting::LightingView& view,
        const SdfExtraGeometry* sdfExtra = nullptr);

private:
    struct RetiredRecords
    {
        std::unique_ptr<rhi::Buffer> buffer;
        u64 retireAtTick{0U};
    };

    rhi::Device& device_;
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
    std::vector<RetiredRecords> records_;
    u64 tick_{0U};
    MeshSdfScene* sdfScene_{nullptr};
};
} // namespace orbit::mesh_render
