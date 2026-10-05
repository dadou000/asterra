#pragma once

#include <orbit/lighting/LightingView.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::lighting
{
class RadianceCacheSampler
{
public:
    RadianceCacheSampler(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void ResolveFallback(
        rhi::CommandList& commands,
        rhi::Texture& indirect,
        rhi::Texture& surfaceBaseRoughness,
        rhi::Texture& surfaceNormalMetallic,
        // Class 3 (authored proxy) pixels are skipped: they are lit exactly.
        rhi::Texture& surfaceEmissionClass,
        rhi::Texture& depth,
        rhi::Buffer& radianceCells,
        rhi::Buffer& radianceLevels,
        u32 levelCount,
        u32 width,
        u32 height,
        const LightingView& view,
        f32 cacheStrength = 1.0F);
private:
    std::unique_ptr<rhi::ComputePipeline> pipeline_;
};
} // namespace orbit::lighting
