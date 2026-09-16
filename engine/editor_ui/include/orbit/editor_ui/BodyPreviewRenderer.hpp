#pragma once

#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <memory>

namespace orbit::editor_ui
{
// Lightweight but real editor body preview. It renders analytic sphere/
// ellipsoid reference shapes directly; terrain-capable views can replace
// this with the full surface renderer without changing the panel contract.
class BodyPreviewRenderer
{
public:
    BodyPreviewRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);
    ~BodyPreviewRenderer();

    BodyPreviewRenderer(
        const BodyPreviewRenderer&) = delete;
    BodyPreviewRenderer& operator=(
        const BodyPreviewRenderer&) = delete;

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& target,
        u32 width,
        u32 height,
        const universe::BodyShape& shape,
        const render_view::CameraState& camera);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::editor_ui
