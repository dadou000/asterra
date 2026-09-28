#pragma once

#include <orbit/studio_ui/InspectorProviderRegistry.hpp>
#include <orbit/studio_ui/StudioPersistentState.hpp>
#include <orbit/studio_ui/StudioUiContributions.hpp>
#include <orbit/studio_ui/ViewportAuthoringState.hpp>

#include <filesystem>
#include <string>

namespace orbit::studio_session
{
class StudioSession;
}

namespace orbit::studio_ui
{
class StudioViewportPanels;

// Shared extension registries used by Orbit and hot-reloadable plugins. They
// intentionally contain presentation contributions only; commands, scene
// objects and authoring state remain owned by their existing services.
[[nodiscard]] InspectorProviderRegistry&
GlobalInspectorProviders() noexcept;

[[nodiscard]] StudioUiContributionRegistry&
GlobalStudioUiContributions() noexcept;

// Modular shell integration for features that should not enlarge the viewport
// panel implementation: selection breadcrumbs, command search, quick-create,
// viewport layout/gizmo presentation, plugin toolbar contributions and
// project-local presentation persistence.
class StudioExpansionShell
{
public:
    explicit StudioExpansionShell(
        StudioViewportPanels& owner) noexcept;
    ~StudioExpansionShell();

    StudioExpansionShell(const StudioExpansionShell&) = delete;
    StudioExpansionShell& operator=(const StudioExpansionShell&) = delete;

    [[nodiscard]] ViewportAuthoringState&
    ViewportState() noexcept
    {
        return viewportState_;
    }

    [[nodiscard]] const ViewportAuthoringState&
    ViewportState() const noexcept
    {
        return viewportState_;
    }

private:
    void DrawNavigationBand(editor_ui::PanelContext& context);
    void DrawViewportBand(editor_ui::PanelContext& context);
    void DrawInspectorExtension(editor_ui::PanelContext& context);
    void DrawContributions(
        editor_ui::PanelContext& context,
        StudioContributionSurface surface);

    // Loads when the bound project changes and writes only when one of the
    // expansion-owned fields changes. Workspace/browser/activity fields are
    // preserved verbatim so the same StudioPersistentState file can become
    // the single authority as those surfaces are migrated later.
    void SyncPersistentState() noexcept;
    void SavePersistentStateIfChanged() noexcept;

    StudioViewportPanels* owner_{nullptr};
    ViewportAuthoringState viewportState_{};
    std::string commandQuery_;
    bool commandSearchOpen_{false};
    bool quickCreateOpen_{false};
    bool attached_{false};

    studio_session::StudioSession* persistentSession_{nullptr};
    std::filesystem::path persistentStatePath_;
    StudioPersistentState persistentState_{};
    std::string persistentSnapshot_;
    bool persistentStateLoaded_{false};
};
} // namespace orbit::studio_ui
