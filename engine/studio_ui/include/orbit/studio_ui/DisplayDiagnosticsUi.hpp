#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>

#include <string_view>

namespace orbit::content
{
class ContentService;
}

namespace orbit::studio_session
{
class StudioSession;
}

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

    // M41 selection inspection remains an explicit Studio binding. The
    // diagnostics panel can still be constructed in renderer-only tools/tests.
    void BindLightingInspection(
        studio_session::StudioSession& session,
        content::ContentService& content) noexcept
    {
        inspectionSession_ = &session;
        inspectionContent_ = &content;
    }

    void ClearLightingInspectionBinding() noexcept
    {
        inspectionSession_ = nullptr;
        inspectionContent_ = nullptr;
    }

    void Register(
        editor_ui::EditorUi& ui);

private:
    // M40 preserves the M23-M29 diagnostics/control implementation and layers
    // project defaults + session lighting overrides around it.
    void RegisterBase(
        editor_ui::EditorUi& ui);
    void DrawViewportBase(
        editor_ui::PanelContext& context,
        std::string_view viewportId,
        std::string_view label);

    void DrawViewport(
        editor_ui::PanelContext& context,
        std::string_view viewportId,
        std::string_view label);

    StudioViewportRenderer* renderer_{nullptr};
    studio_session::StudioSession* inspectionSession_{nullptr};
    content::ContentService* inspectionContent_{nullptr};
};
} // namespace orbit::studio_ui
