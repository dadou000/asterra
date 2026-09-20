#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_session/ProjectBrowserModel.hpp>
#include <orbit/studio_session/ProjectSettingsModel.hpp>
#include <orbit/studio_session/StudioTerrainRoundTripVerifier.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace orbit::studio_ui
{
// M20A project/world panels. This class owns presentation buffers only;
// StudioWorkspace/ProjectDocument/EditorWorldSession remain authoritative.
// The instance must outlive the EditorUi panel registrations it installs.
class ProjectAuthoringUi
{
public:
    ProjectAuthoringUi(
        studio_session::StudioWorkspace& workspace,
        std::filesystem::path recentProjectsFile);

    void Register(editor_ui::EditorUi& ui);
    void RegisterProjectSettings(
        editor_ui::EditorUi& ui);
    void RegisterWorldDocuments(
        editor_ui::EditorUi& ui,
        bool allowCloseWorld = true);

    // Called synchronously after a successful project create/open/close. This
    // lets the application rebuild project-bound GPU/session presentation in
    // the same UI frame before any other panel can observe stale references.
    void SetWorkspaceChangedCallback(
        std::function<void()> callback);

    inline static constexpr editor_ui::PanelId kProjectBrowserPanel{
        .high = 0x4f52424954535455ULL,
        .low = 0x50524f4a42525753ULL
    };

    inline static constexpr editor_ui::PanelId kProjectSettingsPanel{
        .high = 0x4f52424954535455ULL,
        .low = 0x50524f4a53455454ULL
    };

    inline static constexpr editor_ui::PanelId kWorldDocumentsPanel{
        .high = 0x4f52424954535455ULL,
        .low = 0x574f524c44444f43ULL
    };

private:
    void DrawProjectBrowser(editor_ui::PanelContext& context);
    void DrawProjectSettings(editor_ui::PanelContext& context);
    void DrawWorldDocuments(editor_ui::PanelContext& context);
    void SynchronizeProjectBuffers();
    void NotifyWorkspaceChanged();

    studio_session::StudioWorkspace* workspace_{nullptr};
    studio_session::ProjectBrowserModel projectBrowser_;
    studio_session::ProjectSettingsModel projectSettings_;
    std::function<void()> workspaceChanged_;

    u64 observedWorkspaceGeneration_{~u64{0}};
    std::string newProjectRoot_;
    std::string newProjectName_{"New Orbit Project"};
    std::string openProjectPath_;
    std::string projectDisplayName_;

    std::string createWorldPath_{"Worlds/NewWorld.orbitworld"};
    std::string createWorldName_{"New World"};
    std::optional<std::filesystem::path> selectedWorld_;
    std::string selectedWorldName_;
    std::optional<
        studio_session::StudioTerrainRoundTripReport>
        terrainRoundTripReport_;
    u64 terrainRoundTripReportGeneration_{~u64{0}};
    std::string status_;
    bool allowCloseWorld_{true};
};
} // namespace orbit::studio_ui
