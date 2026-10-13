#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::studio_ui
{
// Transient post-display diagnostics. The full-screen debug reads the same
// scene and surface attachments as the lighting pass, but cannot be hidden by
// GI, clouds, eye adaptation, or the HDR output transform.
struct SunAuditViewportInputs
{
    rhi::Texture* scene{nullptr};
    rhi::Texture* emissionMetadata{nullptr};
    rhi::Texture* depth{nullptr};
    rhi::Buffer* terrainShadow{nullptr};
    render_view::CameraState camera{};
    math::Float3 sunDirection{};
    f32 planetRadiusMeters{0.0F};
    f32 sunAngularRadiusRadians{0.0F};
};

class SunAuditViewportIndicator
{
public:
    SunAuditViewportIndicator(rhi::Device& device, const shader::Compiler& compiler);
    void Draw(rhi::CommandList& commands, rhi::Texture& display,
              const SunAuditViewportInputs& inputs,
              u32 width, u32 height, u8 method, bool debug) const;
private:
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> diagnosticPipeline_;
    std::unique_ptr<rhi::Buffer> emptyTerrainShadow_;
};
} // namespace orbit::studio_ui
