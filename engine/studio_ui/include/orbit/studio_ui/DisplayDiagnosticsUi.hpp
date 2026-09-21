#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>

#include <string_view>

namespace orbit::studio_ui
{
class DisplayDiagnosticsUi
{
public:
    static constexpr editor_ui::PanelId kPanelId{
        .high = 0x4f52424954444953ULL,
        .low = 0x504c415930303031ULL
    };

    explicit DisplayDiagnosticsUi(
        StudioViewportRenderer& renderer) noexcept;

    void Register(
        editor_ui::EditorUi& ui);

private:
    void DrawViewport(
        editor_ui::PanelContext& context,
        std::string_view viewportId,
        std::string_view label);

    StudioViewportRenderer* renderer_{nullptr};
};
} // namespace orbit::studio_ui
