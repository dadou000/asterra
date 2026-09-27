#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>

namespace orbit::studio_ui
{
// A single toolbar that switches what every Studio viewport's GBuffer shows
// (lit / albedo+roughness / normal+metallic / emission+metadata), instead of
// requiring the per-viewport "Surface View" row already in each Viewport
// panel. Both entry points call the same StudioRenderViewSet setter that
// view.surface_debug_set drives over RPC/MCP (ORBIT_UI_RULES.md section 13).
class DebugViewUi
{
public:
    explicit DebugViewUi(StudioRenderViewSet& views) noexcept;

    void Register(editor_ui::EditorUi& ui);

    inline static constexpr editor_ui::PanelId kPanelId{
        .high = 0x4f52424954535455ULL,
        .low = 0x4445425547564945ULL
    };

private:
    void Draw(editor_ui::PanelContext& context);
    void DrawViewportRow(
        editor_ui::PanelContext& context,
        const StudioRenderViewInfo& info);

    StudioRenderViewSet* views_{nullptr};
};
} // namespace orbit::studio_ui
