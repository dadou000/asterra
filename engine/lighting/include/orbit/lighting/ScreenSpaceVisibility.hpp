#pragma once

#include <orbit/lighting/LightingView.hpp>
#include <orbit/lighting/Visibility.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::lighting
{
struct ScreenSpaceVisibilityConfig
{
    u32 maximumSteps{64U};
    f32 minimumStepMeters{0.05F};
    f32 maximumStepMeters{2.0F};
    f32 thicknessMeters{0.15F};
    f32 edgeFadeUv{0.04F};
};

class ScreenSpaceVisibilityBatch
{
public:
    ScreenSpaceVisibilityBatch(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void Dispatch(
        rhi::CommandList& commands,
        rhi::Buffer& queries,
        rhi::Buffer& results,
        rhi::Texture& depth,
        rhi::Texture& surfaceNormalMetallic,
        u32 queryCount,
        u32 viewportWidth,
        u32 viewportHeight,
        const LightingView& view,
        const ScreenSpaceVisibilityConfig& config = {});

private:
    std::unique_ptr<rhi::ComputePipeline> pipeline_;
};
} // namespace orbit::lighting
