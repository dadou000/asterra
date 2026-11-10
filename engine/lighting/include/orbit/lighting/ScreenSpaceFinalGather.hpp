#pragma once

#include <orbit/lighting/LightingView.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <array>
#include <memory>

namespace orbit::lighting
{
struct ScreenSpaceFinalGatherSettings
{
    f32 radiusMeters{3.0F};
    f32 thicknessMeters{0.12F};
    f32 intensity{1.0F};
    f32 temporalWeight{0.88F};
    f32 historyDepthTolerance{0.015F};
    f32 historyNormalThreshold{0.90F};
    u32 stepsPerRay{10U};
};

[[nodiscard]] bool CanReuseFinalGatherHistory(
    const LightingView& previous,
    const LightingView& current) noexcept;

// The merged mesh distance field the gather falls back to for rays the screen
// cannot resolve (see mesh_render::MeshSdfScene). Buffers are borrowed.
struct SdfGatherInput
{
    // The corner-packed distance field (MeshSdfScene's distanceCorners), not
    // the plain f32 volume.
    rhi::Buffer* distance{nullptr};
    rhi::Buffer* albedo{nullptr};
    rhi::Buffer* normal{nullptr};
    rhi::Buffer* radiance{nullptr};
    // Frame coordinates of voxel (0,0,0)'s centre.
    math::Double3 originInFrameMeters{};
    f32 voxelSize{0.25F};
    std::array<u32, 3> dimensions{};
};

class ScreenSpaceFinalGatherRenderer
{
public:
    ScreenSpaceFinalGatherRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void Gather(
        rhi::CommandList& commands,
        rhi::Texture& sceneColor,
        rhi::Texture& surfaceBaseRoughness,
        rhi::Texture& surfaceNormalMetallic,
        rhi::Texture& surfaceEmissionClass,
        rhi::Texture& depth,
        rhi::Texture& previousIndirect,
        rhi::Texture& previousMeta,
        rhi::Texture& currentIndirect,
        rhi::Texture& currentMeta,
        u32 width,
        u32 height,
        const LightingView& view,
        bool historyCompatible,
        rhi::Buffer* particleLightGrid = nullptr,
        const ScreenSpaceFinalGatherSettings& settings = {},
        const SdfGatherInput* sdf = nullptr,
        // 4 uint counters (surface, smooth, uncovered, mirror-like pixels); nullptr = none.
        rhi::Buffer* needStats = nullptr);

    void Combine(
        rhi::CommandList& commands,
        rhi::Texture& sceneColor,
        rhi::Texture& indirect,
        rhi::Texture& target,
        u32 width,
        u32 height,
        f32 intensity = 1.0F,
        // Writes the gather's coverage (confidence / brightness, magenta = nothing) instead of
        // adding it to the scene colour.
        bool coverageView = false,
        // Writes only the indirect light (final gather + radiance cache cascades),
        // replacing the scene colour. Ignored when coverageView is set.
        bool indirectOnlyView = false);

private:
    std::unique_ptr<rhi::ComputePipeline> gatherPipeline_;
    std::unique_ptr<rhi::ComputePipeline> combinePipeline_;
    std::unique_ptr<rhi::Buffer> dummyParticleLightGrid_;
    std::unique_ptr<rhi::Buffer> dummySdf_;
    // Frame counter feeding the per-frame sample rotation.
    u32 frameCounter_{0U};
};
} // namespace orbit::lighting
