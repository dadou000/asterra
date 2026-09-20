#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_ui
{
inline constexpr editor_ui::PanelId kPrimaryViewportPanel{
    .high = 0x4f52424954535455ULL,
    .low = 0x44494f5657455750ULL
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
    StudioViewportPanels() = default;

    StudioViewportPanels(
        StudioRenderViewSet& views,
        studio_session::StudioSession& session) noexcept;

    void Rebind(
        StudioRenderViewSet& views,
        studio_session::StudioSession& session);
    void ClearBinding() noexcept;

    void Register(editor_ui::EditorUi& ui);
    void RegisterSecondary(editor_ui::EditorUi& ui);

private:
    void DrawView(
        editor_ui::PanelContext& context,
        std::string_view id);

    StudioRenderViewSet* views_{nullptr};
    studio_session::StudioSession* session_{nullptr};
    std::string status_;

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
};
} // namespace orbit::studio_ui
