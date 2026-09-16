#pragma once

#include <orbit/frames/FrameGraph.hpp>
#include <orbit/path_geometry/PathDerived.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/time/SimulationTime.hpp>

#include <memory>
#include <span>

namespace orbit::editor_ui
{
// Editor/runtime preview renderer for M20 derived path products. It consumes
// disposable CPU products and converts their frame-local vertices to
// camera-relative float coordinates at draw time.
class PathPreviewRenderer
{
public:
    PathPreviewRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);
    ~PathPreviewRenderer();

    PathPreviewRenderer(
        const PathPreviewRenderer&) = delete;
    PathPreviewRenderer& operator=(
        const PathPreviewRenderer&) = delete;

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& target,
        u32 width,
        u32 height,
        const render_view::CameraState& camera,
        const frames::FrameGraph& frames,
        time::SimulationTime atTime,
        std::span<
            const path_geometry::
                PathDerivedProduct* const>
            products,
        bool drawDebugLines = true);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::editor_ui
