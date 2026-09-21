#pragma once

#include <orbit/lighting/LightingView.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::lighting
{
class ExactReflectionQueryRenderer
{
public:
    ExactReflectionQueryRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void BuildQueries(
        rhi::CommandList& commands,
        rhi::Texture& surfaceBaseRoughness,
        rhi::Texture& surfaceNormalMetallic,
        rhi::Texture& depth,
        rhi::Buffer& queries,
        rhi::Buffer& pixelMap,
        rhi::Buffer& counter,
        u32 maximumQueries,
        u32 width,
        u32 height,
        const LightingView& view,
        f32 mirrorRoughness,
        f32 traceRadiusMeters,
        f32 thicknessMeters,
        u32 screenSteps);

    void ResolveResults(
        rhi::CommandList& commands,
        rhi::Texture& targetSceneColor,
        rhi::Texture& surfaceBaseRoughness,
        rhi::Texture& surfaceNormalMetallic,
        rhi::Buffer& results,
        rhi::Buffer& pixelMap,
        rhi::Buffer& radianceCells,
        rhi::Buffer& radianceLevels,
        u32 radianceLevelCount,
        u32 maximumQueries,
        u32 width,
        u32 height,
        f32 cacheStrength = 1.0F);

private:
    std::unique_ptr<rhi::ComputePipeline> buildPipeline_;
    std::unique_ptr<rhi::ComputePipeline> resolvePipeline_;
};
} // namespace orbit::lighting
