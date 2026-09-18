#pragma once

#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_session/StudioSession.hpp>

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

    inline static constexpr editor_ui::PanelId kPanelId{
        .high = 0x4f52424954535455ULL,
        .low = 0x50524f4a53455454ULL
    };

private:
    void Draw(editor_ui::PanelContext& context);
    void SynchronizeAuthority();

    documents::ProjectDocument* project_{nullptr};
    studio_session::StudioSession* session_{nullptr};
    std::string displayName_;
    std::string observedDisplayName_;
    std::string status_;
};
} // namespace orbit::studio_ui
