#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::studio_ui
{
// A post-output-transform witness, not a second lighting implementation.
// The marker proves which temporary method reached the actual display image,
// even if the world is drawn forward and bypasses the surface G-buffer.
class SunAuditViewportIndicator
{
public:
    SunAuditViewportIndicator(rhi::Device& device, const shader::Compiler& compiler);
    void Draw(rhi::CommandList& commands, rhi::Texture& display,
              u32 width, u32 height, u8 method, bool debug) const;
private:
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
};
} // namespace orbit::studio_ui
