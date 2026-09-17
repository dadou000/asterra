#include <orbit/studio_ui/StudioUiBundle.hpp>

#include <stdexcept>
#include <utility>

namespace orbit::studio_ui
{
StudioUiBundle::StudioUiBundle(
    editor_ui::EditorUi& ui,
    rhi::Device& device,
    const shader::Compiler& compiler,
    studio_session::StudioWorkspace& workspace,
    std::filesystem::path recentProjectsFile)
    : device_(&device),
      workspace_(&workspace),
      projectAuthoring_(
          workspace,
          std::move(recentProjectsFile)),
      viewportRenderer_(
          device,
          compiler)
{
    projectAuthoring_.SetWorkspaceChangedCallback(
        [this]()
        {
            static_cast<void>(SynchronizeProject());
        });

    projectAuthoring_.Register(ui);
    viewportPanels_.Register(ui);
    static_cast<void>(SynchronizeProject());
}

bool StudioUiBundle::SynchronizeProject()
{
    if (workspace_ == nullptr ||
        device_ == nullptr)
    {
        return false;
    }

    const u64 generation =
        workspace_->Generation();

    if (generation == observedWorkspaceGeneration_)
    {
        return false;
    }

    // Registered panel callbacks outlive individual projects. Remove their
    // old pointers before destroying presentation objects that were bound to
    // the previous StudioSession.
    viewportPanels_.ClearBinding();
    views_.reset();
    runtime_.reset();

    observedWorkspaceGeneration_ = generation;

    if (!workspace_->HasProject())
    {
        return true;
    }

    auto& session = workspace_->Session();

    runtime_ =
        std::make_unique<studio_session::StudioRuntimeBinding>(
            session);
    views_ =
        std::make_unique<StudioRenderViewSet>(
            *device_,
            session);

    viewportPanels_.Rebind(
        *views_,
        session);
    return true;
}

std::optional<studio_session::StudioRuntimeSnapshot>
StudioUiBundle::Refresh()
{
    static_cast<void>(SynchronizeProject());

    if (runtime_ == nullptr ||
        views_ == nullptr)
    {
        return std::nullopt;
    }

    auto snapshot = runtime_->Refresh();
    static_cast<void>(
        views_->Refresh(snapshot));
    return snapshot;
}

std::vector<StudioRenderedView>
StudioUiBundle::Compose(
    render_graph::RenderGraph& graph,
    const studio_session::StudioRuntimeSnapshot& snapshot,
    const time::SimulationTime atTime,
    const bool drawPathDebug)
{
    static_cast<void>(SynchronizeProject());

    if (runtime_ == nullptr ||
        views_ == nullptr ||
        workspace_ == nullptr ||
        !workspace_->HasProject())
    {
        return {};
    }

    if (!runtime_->IsCurrent(snapshot))
    {
        throw std::logic_error(
            "Studio UI bundle received a stale runtime snapshot after a project/world change.");
    }

    return viewportRenderer_.Compose(
        graph,
        *views_,
        workspace_->Session(),
        *runtime_,
        snapshot,
        atTime,
        drawPathDebug);
}

StudioRenderViewSet* StudioUiBundle::Views() noexcept
{
    return views_.get();
}

const StudioRenderViewSet*
StudioUiBundle::Views() const noexcept
{
    return views_.get();
}

studio_session::StudioRuntimeBinding*
StudioUiBundle::Runtime() noexcept
{
    return runtime_.get();
}

const studio_session::StudioRuntimeBinding*
StudioUiBundle::Runtime() const noexcept
{
    return runtime_.get();
}

u64 StudioUiBundle::ObservedWorkspaceGeneration() const noexcept
{
    return observedWorkspaceGeneration_;
}
} // namespace orbit::studio_ui
