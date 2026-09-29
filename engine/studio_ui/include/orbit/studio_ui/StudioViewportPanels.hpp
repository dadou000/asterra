#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_ui/StudioExpansionShell.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::content
{
class ContentService;
}

namespace orbit::studio_ui
{
inline constexpr editor_ui::PanelId kPrimaryViewportPanel{
    .high = 0x4f52424954535455ULL,
    .low = 0x44494f5657455750ULL
};

inline constexpr editor_ui::PanelId kContextInspectorPanel{
    .high = 0x4f52424954535455ULL,
    .low = 0x434f4e54494e5350ULL
};

enum class StudioTerrainAuthoringTool : u8
{
    Select,
    Raise,
    Lower,
    Protection,
    Drainage,
    Canyon,
    Ridge,
    Material,
    BiomePaint
};

inline constexpr editor_ui::PanelId kSecondaryViewportPanel{
    .high = 0x4f52424954535455ULL,
    .low = 0x44494f5657455732ULL
};

// Dockable presentation for the two default independent Studio RenderViews.
// Panel registrations may outlive a project/session; Rebind() swaps only the
// project-bound presentation services while the panel object itself remains
// stable across StudioWorkspace project replacement.
class StudioViewportPanels
{
public:
    StudioViewportPanels();

    StudioViewportPanels(
        StudioRenderViewSet& views,
        studio_session::StudioSession& session) noexcept;

    ~StudioViewportPanels();

    void Rebind(
        StudioRenderViewSet& views,
        studio_session::StudioSession& session);
    void ClearBinding() noexcept;

    void Register(editor_ui::EditorUi& ui);
    void RegisterSecondary(editor_ui::EditorUi& ui);

    void SetContentService(content::ContentService* content) noexcept
    {
        content_ = content;
    }

    void RequestCommandPaletteOpen() noexcept
    {
        expansion_.RequestCommandPaletteOpen();
    }

private:
    friend class StudioExpansionShell;

    struct ViewportModeCommandState;

    void RegisterBase(editor_ui::EditorUi& ui);
    void RegisterSecondaryBase(editor_ui::EditorUi& ui);

    void DrawViewBase(
        editor_ui::PanelContext& context,
        std::string_view id);

    void DrawView(
        editor_ui::PanelContext& context,
        std::string_view id);

    void RegisterContextInspector(
        editor_ui::EditorUi& ui);
    void DrawContextInspector(
        editor_ui::PanelContext& context);

    // StudioExpansionShell owns the two permanent top rows and calls the
    // workspace/context fragments below. This class registers only the bottom
    // activity/status strip, which controls existing bottom-docked views
    // without duplicating their build/log/diagnostic state.
    void RegisterShellBands(
        editor_ui::EditorUi& ui);
    void DrawWorkspaceBand(
        editor_ui::PanelContext& context);
    void DrawContextBand(
        editor_ui::PanelContext& context);
    void DrawActivityBand(
        editor_ui::PanelContext& context);

    [[nodiscard]] i32 QuickCreateCommandPriority(
        std::string_view category) const noexcept;
    [[nodiscard]] bool PreferCommandQuickCreate() const noexcept;
    [[nodiscard]] bool ShowViewportQuickCreate() const noexcept;

    // View-mode commands remain world-registry commands for command search,
    // MCP and automation. Row 2 invokes the same commands through one compact
    // selector instead of registering three permanent toolbar contributions.
    // They are reinstalled after a world switch because EditorWorldSession
    // intentionally replaces its complete command graph with the new world.
    void EnsureViewportModeCommands();
    void UnregisterViewportModeCommands() noexcept;
    void InvokeViewportMode(studio_session::ViewportMode mode);

    // Built-in quick creation remains presentation-aware because placement is
    // intentionally relative to a viewport camera. StudioExpansionShell calls
    // these from + Add so creation no longer needs permanent viewport chrome.
    [[nodiscard]] bool CanCreateAtViewport(
        std::string_view id) const noexcept;
    void CreateLocalLightAtViewport(
        std::string_view id,
        bool spot);
    void CreateVisibilityProxyAtViewport(
        std::string_view id,
        bool box);

    StudioRenderViewSet* views_{nullptr};
    studio_session::StudioSession* session_{nullptr};
    content::ContentService* content_{nullptr};
    std::string status_;
    std::shared_ptr<ViewportModeCommandState> viewportModeCommandState_;

    StudioTerrainAuthoringTool terrainTool_{
        StudioTerrainAuthoringTool::Select};
    f64 terrainBrushInnerRadiusMeters_{250.0};
    f64 terrainBrushOuterRadiusMeters_{1'000.0};
    f64 terrainBrushHeightMeters_{100.0};
    f64 terrainProtection_{0.9};
    f64 terrainDrainageGuidance_{1.0};

    f64 terrainSplineHalfWidthMeters_{500.0};
    f64 terrainSplineFalloffMeters_{500.0};
    f64 terrainSplineHeightMeters_{200.0};
    std::vector<math::Double3> terrainSplinePoints_;
    std::optional<scene::ObjectId> terrainSplineTerrain_;

    terrain_biome::BiomeAuthoredWeightOperation
        biomePaintOperation_{
            terrain_biome::BiomeAuthoredWeightOperation::Replace};
    f64 biomeBrushInnerRadiusMeters_{250.0};
    f64 biomeBrushOuterRadiusMeters_{1'000.0};
    f64 biomeBrushValue_{1.0};
    f64 biomeBrushOpacity_{1.0};
    bool biomeAutomaticOverlay_{false};
    std::optional<f64> hoveredBiomeAuthoredWeight_;
    std::optional<f64> hoveredBiomeAutomaticWeight_;

    // The legacy expert Inspector still owns only its schema presentation
    // preference. Specialized authoring sections are drawn from the same
    // shared provider registry used by canonical Properties.
    bool contextualAdvancedProperties_{false};

    // Owns navigation/command + contextual/viewport presentation rows,
    // project-local shell persistence and plugin contribution registries.
    StudioExpansionShell expansion_{*this};
};
} // namespace orbit::studio_ui
