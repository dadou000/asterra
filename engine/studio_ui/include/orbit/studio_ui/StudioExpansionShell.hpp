#pragma once

#include <orbit/studio_ui/InspectorProviderRegistry.hpp>
#include <orbit/studio_ui/StudioPersistentState.hpp>
#include <orbit/studio_ui/StudioUiContributions.hpp>
#include <orbit/studio_ui/ViewportAuthoringState.hpp>

#include <filesystem>
#include <string>
#include <string_view>

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

    // Presentation extensions use the exact same focus-aware target as the
    // built-in shell. This does not expose or duplicate viewport runtime state.
    [[nodiscard]] std::string_view
    ControlledViewportId() const noexcept
    {
        return SelectedViewportId();
    }

private:
    void DrawNavigationBand(editor_ui::PanelContext& context);
    void DrawViewportBand(editor_ui::PanelContext& context);
    void DrawInspectorExtension(editor_ui::PanelContext& context);
    [[nodiscard]] bool DrawContributions(
        editor_ui::PanelContext& context,
        StudioContributionSurface surface,
        bool responsiveOverflow = false,
        bool verticalList = false);

    // Terrain is the densest contextual tool family. Keep the permanent shell
    // to one selector while the canonical Properties panel exposes only the
    // active tool's parameters. The underlying authoring state remains owned
    // by StudioViewportPanels and its production viewport implementation.
    [[nodiscard]] bool TerrainContextRelevant() const noexcept;
    void DrawTerrainContext(editor_ui::PanelContext& context);
    void DrawTerrainToolProperties(editor_ui::PanelContext& context);

    // View presentation follows the last production viewport window that held
    // editor focus. Properties can pin an explicit viewport when needed, but
    // the permanent shell does not require a manual Primary/Body Map selector.
    [[nodiscard]] std::string_view SelectedViewportId() const noexcept;
    [[nodiscard]] bool ViewportControlsRelevant() const noexcept;
    [[nodiscard]] bool BezierContextRelevant() const noexcept;
    [[nodiscard]] bool DrawBuiltInQuickCreate(
        editor_ui::PanelContext& context);
    void DrawViewportTargetProperties(editor_ui::PanelContext& context);
    void DrawViewportDiagnosticsProperties(editor_ui::PanelContext& context);
    void DrawBezierProperties(editor_ui::PanelContext& context);

    // Loads when the bound project changes and writes only when one of the
    // expansion-owned fields changes. Workspace/browser/activity fields are
    // preserved verbatim so the same StudioPersistentState file can become
    // the single authority as those surfaces are migrated later.
    void SyncPersistentState() noexcept;
    void SavePersistentStateIfChanged() noexcept;

    StudioViewportPanels* owner_{nullptr};
    ViewportAuthoringState viewportState_{};
    std::string commandQuery_;
    bool attached_{false};

    // 0 = follow focused viewport, 1 = pin Primary, 2 = pin Body Map.
    i32 viewportControlMode_{0};
    mutable i32 lastFocusedViewportIndex_{0};

    studio_session::StudioSession* persistentSession_{nullptr};
    std::filesystem::path persistentStatePath_;
    StudioPersistentState persistentState_{};
    std::string persistentSnapshot_;
    bool persistentStateLoaded_{false};
};
} // namespace orbit::studio_ui
