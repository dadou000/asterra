#pragma once

#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_session/StudioTerrainRoundTripVerifier.hpp>
#if defined(ORBIT_ENABLE_VALIDATION_TOOLS)
#include <orbit/validation/StudioTerrainValidationScenario.hpp>
#endif

#include <optional>
#include <string>

namespace orbit::studio_ui
{
// Project settings for an already-open Studio project. ProjectDocument remains
// the persisted authority and startup-world changes route through StudioSession.
class ProjectSettingsUi
{
public:
    ProjectSettingsUi(
        documents::ProjectDocument& project,
        studio_session::StudioSession& session);

    void Register(editor_ui::EditorUi& ui);

    [[nodiscard]] studio_session::StudioTerrainRoundTripReport
    RunTerrainRoundTripValidation();

#if defined(ORBIT_ENABLE_VALIDATION_TOOLS)
    [[nodiscard]] studio_session::StudioTerrainValidationScenarioReport
    RunTerrainValidationScenario();
#endif

    inline static constexpr editor_ui::PanelId kPanelId{
        .high = 0x4f52424954535455ULL,
        .low = 0x50524f4a53455454ULL
    };

private:
    // M40 wraps the existing project panel rather than creating a separate
    // settings application or duplicating the terrain/project workflow.
    void RegisterBase(editor_ui::EditorUi& ui);
    void DrawBase(editor_ui::PanelContext& context);

    void Draw(editor_ui::PanelContext& context);
    void SynchronizeAuthority();

    documents::ProjectDocument* project_{nullptr};
    studio_session::StudioSession* session_{nullptr};
    std::string displayName_;
    std::string observedDisplayName_;
    std::optional<
        studio_session::StudioTerrainRoundTripReport>
        terrainRoundTripReport_;
#if defined(ORBIT_ENABLE_VALIDATION_TOOLS)
    std::optional<
        studio_session::StudioTerrainValidationScenarioReport>
        terrainValidationScenarioReport_;
#endif
    std::string status_;
};
} // namespace orbit::studio_ui
