#pragma once

#include <orbit/editor_ui/BodyPreviewRenderer.hpp>
#include <orbit/editor_ui/PathPreviewRenderer.hpp>
#include <orbit/render_graph/RenderGraph.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/studio_session/StudioRuntimeBinding.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>
#include <orbit/time/SimulationTime.hpp>

#include <string>
#include <vector>

namespace orbit::studio_ui
{
struct StudioRenderedView
{
    std::string id;
    render_view::ImportedTargets targets{};
    bool targeted{false};
};

// Adds one body/path preview composition per owned Studio RenderView. Each pass
// resolves the body from that view's own logical target, so pinned map/debug
// views do not depend on a global active body.
class StudioViewportRenderer
{
public:
    StudioViewportRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    [[nodiscard]] std::vector<StudioRenderedView> Compose(
        render_graph::RenderGraph& graph,
        StudioRenderViewSet& views,
        studio_session::StudioSession& session,
        studio_session::StudioRuntimeBinding& runtime,
        const studio_session::StudioRuntimeSnapshot& snapshot,
        time::SimulationTime atTime = {},
        bool drawPathDebug = true);

private:
    editor_ui::BodyPreviewRenderer bodyRenderer_;
    editor_ui::PathPreviewRenderer pathRenderer_;
};
} // namespace orbit::studio_ui
