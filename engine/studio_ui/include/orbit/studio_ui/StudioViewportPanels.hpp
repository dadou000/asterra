#pragma once

#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_ui/StudioAssetShelf.hpp>
#include <orbit/studio_ui/StudioExpansionShell.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>
#include <orbit/studio_ui/StudioTextDiagnosticsHud.hpp>
#include <orbit/studio_ui/StudioViewportManipulatorUi.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

        std::filesystem::path shelfState;
        if (content != nullptr && session_ != nullptr)
        {
            try
            {
                shelfState =
                    session_->World().Project().RootDirectory() /
                    ".orbit" /
                    "AssetShelf.ini";
            }
            catch (...)
            {
                shelfState.clear();
            }
        }

        InstallStudioAssetShelf(content, std::move(shelfState));
        InstallQol(nullptr);
    }

    void RequestCommandPaletteOpen() noexcept
    {
        expansion_.RequestCommandPaletteOpen();
    }

    void SetEcoModeAccessors(
        std::function<bool()> getter,
        std::function<void(bool)> setter)
    {
        ecoModeGetter_ = std::move(getter);
        ecoModeSetter_ = std::move(setter);
    }

    // Shared operation behind the Scene toolbar, F shortcut and RPC.
    [[nodiscard]] bool FrameSelectedObject();

    // Shows the horizontal toolbar that belongs to the active workspace mode
    // (Scene today) and removes it for modes that have none.
    void SyncModeToolbar();

    // Workspace mode (Scene, Planet, Celestial, Simulation, Shading) and the
    // element-bubble popover are reachable from RPC/MCP as well as the UI.
    [[nodiscard]] std::string_view WorkspaceModeName() const noexcept;
    [[nodiscard]] bool SetWorkspaceMode(std::string_view name);
    void RequestElementBubble(scene::ObjectId object) noexcept
    {
        bubbleOpenRequest_ = object;
    }

    // Move / Rotate / Scale handles of the single selected object over the
    // perspective viewport `id`, driven by the Scene toolbar's tool, space and
    // snap settings. Call right after the viewport Image is submitted.
    // Returns true while the handles own the left button (hovering one or
    // dragging), so that press must not also select or place something.
    // The same operation without a pointer is the object.transform RPC.
    [[nodiscard]] bool HandleViewportGizmo(
        editor_ui::PanelContext& context,
        std::string_view id);

    void SetGizmoRelativeMouseDelta(math::Float2 delta) noexcept
    {
        gizmoRelativeMouseDelta_ = delta;
    }
    [[nodiscard]] bool GizmoDragging() const noexcept;

private:
    friend class StudioExpansionShell;

    struct ViewportModeCommandState;
    math::Float2 gizmoRelativeMouseDelta_{};
    std::function<bool()> ecoModeGetter_;
    std::function<void(bool)> ecoModeSetter_;

    void RegisterBase(editor_ui::EditorUi& ui);
    void RegisterSecondaryBase(editor_ui::EditorUi& ui);

    void DrawViewBase(
        editor_ui::PanelContext& context,
        std::string_view id);

    void DrawView(
        editor_ui::PanelContext& context,
        std::string_view id);

    void RegisterContextInspector(editor_ui::EditorUi& ui);
    void DrawContextInspector(editor_ui::PanelContext& context);

    void InstallQol(editor_ui::EditorUi* ui = nullptr) noexcept;
    void EnsureQolCommands();
    void TrackSelectionHistory();
    void NavigateSelectionHistory(i32 delta);
    void SelectParent();
    void SelectFirstChild();
    void DuplicateSelection();
    void DeleteSelection();
    void BeginRenameSelection();
    void CommitRenameSelection();
    void RevealSelectionInWorld();
    void DrawQolNavigation(editor_ui::PanelContext& context);
    void DrawQolHistory(editor_ui::PanelContext& context);
    void DrawQolProperties(editor_ui::PanelContext& context);
    void ApplyViewportPreset(i32 preset);
    void Notify(
        std::string title,
        std::string detail = {},
        bool error = false) noexcept;

    void RegisterShellBands(editor_ui::EditorUi& ui);
    void DrawSceneToolbar(editor_ui::PanelContext& context);
    void DrawCelestialToolbar(editor_ui::PanelContext& context);

    // Small "v" chip that opens a popover with the ordinary (non-advanced)
    // parameters of one object. Edits go through CommandService, so they are
    // undoable and identical to editing the same property in Properties.
    void DrawElementBubble(
        editor_ui::PanelContext& context,
        scene::ObjectId object);
    void DrawWorkspaceBand(editor_ui::PanelContext& context);
    void DrawContextBand(editor_ui::PanelContext& context);
    void DrawActivityBand(editor_ui::PanelContext& context);

    [[nodiscard]] i32 QuickCreateCommandPriority(
        std::string_view category) const noexcept;
    [[nodiscard]] bool PreferCommandQuickCreate() const noexcept;
    [[nodiscard]] bool ShowViewportQuickCreate() const noexcept;

    void EnsureViewportModeCommands();
    void UnregisterViewportModeCommands() noexcept;
    void InvokeViewportMode(studio_session::ViewportMode mode);

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
    editor_ui::EditorUi* ui_{nullptr};
    std::string status_;
    bool celestialMoreRequested_{false};
    std::optional<scene::ObjectId> bubbleOpenRequest_;
    std::shared_ptr<ViewportModeCommandState> viewportModeCommandState_;

    bool qolInstalled_{false};
    u64 observedSelectionRevision_{0};
    std::vector<std::vector<scene::ObjectId>> selectionHistory_;
    std::size_t selectionHistoryCursor_{0};
    bool applyingSelectionHistory_{false};
    bool renameSelection_{false};
    bool renameFocusRequested_{false};
    std::string renameText_;
    std::string propertyFilter_;
    bool propertyModifiedOnly_{false};
    std::vector<schema::PropertyId> pinnedProperties_;
    std::optional<schema::PropertyValue> copiedPropertyValue_;

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
    StudioTextDiagnosticsHud textHud_;
    std::optional<f64> hoveredBiomeAuthoredWeight_;
    std::optional<f64> hoveredBiomeAutomaticWeight_;

    bool contextualAdvancedProperties_{false};

    StudioExpansionShell expansion_{*this};
    StudioViewportManipulatorUi manipulatorUi_;
};
} // namespace orbit::studio_ui

#include <orbit/studio_ui/StudioQol.inl>
