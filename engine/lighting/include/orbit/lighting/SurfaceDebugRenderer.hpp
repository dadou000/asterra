#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::lighting
{
enum class SurfaceDebugMode : u8
{
    Lit,
    BaseColorRoughness,
    NormalMetallic,
    EmissionMetadata
};

class SurfaceDebugRenderer
{
public:
    SurfaceDebugRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& source,
        rhi::Texture& target,
        u32 width,
        u32 height,
        SurfaceDebugMode mode);

private:
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
};
} // namespace orbit::lighting
