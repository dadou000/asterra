#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/render_graph/RenderGraph.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/studio_session/StudioRuntimeBinding.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>
#include <orbit/studio_ui/ProjectAuthoringUi.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>
#include <orbit/studio_ui/StudioViewportPanels.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>
#include <orbit/time/SimulationTime.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace orbit::studio_ui
{
// Stable application-level owner for the M20A project/world UI and project-
// bound GPU viewport stack. The bundle survives StudioWorkspace project
// replacement; only the services that retain a StudioSession reference are
// destroyed and reconstructed when workspace generation changes.
class StudioUiBundle
{
public:
    StudioUiBundle(
        editor_ui::EditorUi& ui,
        rhi::Device& device,
        const shader::Compiler& compiler,
        studio_session::StudioWorkspace& workspace,
        std::filesystem::path recentProjectsFile);

    StudioUiBundle(const StudioUiBundle&) = delete;
    StudioUiBundle& operator=(const StudioUiBundle&) = delete;

    // Rebinds project-scoped viewport/runtime services if the project changed.
    [[nodiscard]] bool SynchronizeProject();

    // Advances the current StudioSession and applies all logical viewport
    // targets to their GPU RenderViews. A project may be absent by design.
    [[nodiscard]] std::optional<studio_session::StudioRuntimeSnapshot>
    Refresh();

    // Composes all active Studio RenderViews for the current project. Callers
    // then consume the returned texture handles in the final UI/swapchain pass.
    [[nodiscard]] std::vector<StudioRenderedView> Compose(
        render_graph::RenderGraph& graph,
        const studio_session::StudioRuntimeSnapshot& snapshot,
        time::SimulationTime atTime = {},
        bool drawPathDebug = true);

    [[nodiscard]] StudioRenderViewSet* Views() noexcept;
    [[nodiscard]] const StudioRenderViewSet* Views() const noexcept;

    [[nodiscard]] studio_session::StudioRuntimeBinding* Runtime() noexcept;
    [[nodiscard]] const studio_session::StudioRuntimeBinding* Runtime() const noexcept;

    [[nodiscard]] u64 ObservedWorkspaceGeneration() const noexcept;

private:
    rhi::Device* device_{nullptr};
    studio_session::StudioWorkspace* workspace_{nullptr};
    u64 observedWorkspaceGeneration_{~u64{0}};

    ProjectAuthoringUi projectAuthoring_;
    StudioViewportRenderer viewportRenderer_;
    StudioViewportPanels viewportPanels_;

    std::unique_ptr<studio_session::StudioRuntimeBinding> runtime_;
    std::unique_ptr<StudioRenderViewSet> views_;
};
} // namespace orbit::studio_ui
