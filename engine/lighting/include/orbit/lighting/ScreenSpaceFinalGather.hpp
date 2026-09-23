#pragma once

#include <orbit/lighting/LightingView.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

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
        const ScreenSpaceFinalGatherSettings& settings = {});

    void Combine(
        rhi::CommandList& commands,
        rhi::Texture& sceneColor,
        rhi::Texture& indirect,
        rhi::Texture& target,
        u32 width,
        u32 height,
        f32 intensity = 1.0F);

private:
    std::unique_ptr<rhi::ComputePipeline> gatherPipeline_;
    std::unique_ptr<rhi::ComputePipeline> combinePipeline_;
    std::unique_ptr<rhi::Buffer> dummyParticleLightGrid_;
};
} // namespace orbit::lighting
