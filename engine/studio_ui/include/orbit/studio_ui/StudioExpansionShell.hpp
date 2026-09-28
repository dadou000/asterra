#pragma once

#include <orbit/studio_ui/InspectorProviderRegistry.hpp>
#include <orbit/studio_ui/StudioUiContributions.hpp>
#include <orbit/studio_ui/ViewportAuthoringState.hpp>

#include <string>

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
// viewport layout/gizmo presentation and plugin toolbar contributions.
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
    void DrawContributions(
        editor_ui::PanelContext& context,
        StudioContributionSurface surface);

    StudioViewportPanels* owner_{nullptr};
    ViewportAuthoringState viewportState_{};
    std::string commandQuery_;
    bool commandSearchOpen_{false};
    bool quickCreateOpen_{false};
    bool attached_{false};
};
} // namespace orbit::studio_ui
