#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::lighting
{
class MaterialEmissionSurfaceOverrideRenderer
{
public:
    MaterialEmissionSurfaceOverrideRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void Apply(
        rhi::CommandList& commands,
        rhi::Texture& surfaceEmissionClass,
        u32 width,
        u32 height,
        math::Float3 emissionRadiance);

private:
    std::unique_ptr<rhi::ComputePipeline> pipeline_;
};
} // namespace orbit::lighting
