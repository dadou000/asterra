#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>

#include <string>
#include <string_view>

namespace orbit::studio_ui
{
inline constexpr editor_ui::PanelId kPrimaryViewportPanel{
    .high = 0x4f52424954535455ULL,
    .low = 0x44494f5657455750ULL
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

private:
    void DrawView(
        editor_ui::PanelContext& context,
        std::string_view id);

    StudioRenderViewSet* views_{nullptr};
    studio_session::StudioSession* session_{nullptr};
    std::string status_;
};
} // namespace orbit::studio_ui
