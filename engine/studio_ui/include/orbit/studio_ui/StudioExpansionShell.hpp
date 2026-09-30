#pragma once

#include <orbit/studio_ui/InspectorProviderRegistry.hpp>
#include <orbit/studio_ui/StudioPersistentState.hpp>
#include <orbit/studio_ui/StudioUiContributions.hpp>
#include <orbit/studio_ui/ViewportAuthoringState.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace orbit::studio_session
{
class StudioSession;
}

namespace orbit::studio_ui
{
class StudioViewportPanels;

[[nodiscard]] InspectorProviderRegistry&
GlobalInspectorProviders() noexcept;

[[nodiscard]] StudioUiContributionRegistry&
GlobalStudioUiContributions() noexcept;

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

    [[nodiscard]] std::string_view
    ControlledViewportId() const noexcept
    {
        return SelectedViewportId();
    }

    void RequestCommandPaletteOpen() noexcept
    {
        commandPaletteOpenRequested_ = true;
    }

    // Keyboard-first + Add entry point. Reuse the proven command palette
    // rather than introducing a second modal: filtering for authoring verbs
    // immediately surfaces Create/Add/New command descriptors, including
    // generated argument forms and project-asset pickers.
    void RequestQuickCreateOpen() noexcept
    {
        commandQuery_ = "Create";
        commandPaletteSelection_ = 0;
        commandPaletteOpenRequested_ = true;
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

    [[nodiscard]] bool TerrainContextRelevant() const noexcept;
    void DrawTerrainContext(editor_ui::PanelContext& context);
    void DrawTerrainToolProperties(editor_ui::PanelContext& context);

    [[nodiscard]] std::string_view SelectedViewportId() const noexcept;
    [[nodiscard]] bool ViewportControlsRelevant() const noexcept;
    [[nodiscard]] bool BezierContextRelevant() const noexcept;
    [[nodiscard]] bool DrawBuiltInQuickCreate(
        editor_ui::PanelContext& context);
    void DrawViewportTargetProperties(editor_ui::PanelContext& context);
    void DrawViewportDiagnosticsProperties(editor_ui::PanelContext& context);
    void DrawBezierProperties(editor_ui::PanelContext& context);

    void SyncPersistentState() noexcept;
    void SavePersistentStateIfChanged() noexcept;

    StudioViewportPanels* owner_{nullptr};
    ViewportAuthoringState viewportState_{};
    std::string commandQuery_;
    i32 commandPaletteSelection_{0};
    bool commandPaletteOpenRequested_{false};
    std::string quickCreateBrowseQuery_;
    i32 quickCreateBrowseSelection_{0};
    commands::CommandId quickCreateArgumentCommand_{};
    commands::CommandArguments quickCreateArguments_;
    std::unordered_map<std::string, bool> quickCreateArgumentEnabled_;
    std::unordered_map<std::string, std::string> quickCreateIdText_;
    std::unordered_map<std::string, std::string> quickCreatePickerQuery_;
    std::unordered_map<std::string, i32> quickCreatePickerSelection_;
    std::string quickCreateArgumentError_;
    bool attached_{false};

    i32 viewportControlMode_{0};
    mutable i32 lastFocusedViewportIndex_{0};

    studio_session::StudioSession* persistentSession_{nullptr};
    std::filesystem::path persistentStatePath_;
    StudioPersistentState persistentState_{};
    std::string persistentSnapshot_;
    bool persistentStateLoaded_{false};
};
} // namespace orbit::studio_ui
