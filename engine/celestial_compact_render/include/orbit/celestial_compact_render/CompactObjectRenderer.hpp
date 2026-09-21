#pragma once

#include <orbit/celestial_compact_objects/CompactObject.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>
#include <optional>

namespace orbit::celestial_compact_render
{
struct CompactObjectDraw
{
    celestial_compact_objects::
        CompactObjectPresentation compact{};
    std::optional<
        celestial_compact_objects::
            AccretionFlowParameters>
        accretion;

    render_view::CameraState camera{};

    f64 projectedShadowRadiusPixels{0.0};
    f64 projectedOpticalRadiusPixels{0.0};

    // Point-proxy transition is expressed as a continuous weight so the same
    // analytic optical model survives into the subpixel regime.
    f32 pointProxyWeight{0.0F};
    f32 opacity{1.0F};
};

class CompactObjectRenderer
{
public:
    CompactObjectRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& target,
        u32 width,
        u32 height,
        const CompactObjectDraw& draw);

private:
    std::unique_ptr<rhi::GraphicsPipeline>
        pipeline_;
};
} // namespace orbit::celestial_compact_render
