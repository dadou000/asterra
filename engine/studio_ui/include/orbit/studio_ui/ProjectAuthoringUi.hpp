#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/platform/FileDialog.hpp>
#include <orbit/studio_session/ProjectBrowserModel.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <future>
#include <string>
#include <vector>
#include <string_view>
#include <vector>

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

    // The Project Browser is the only panel this class registers; Project
    // Settings and World Documents are ProjectSettingsUi and WorldDocumentsUi.
    void RegisterProjectBrowser(
        editor_ui::EditorUi& ui,
        bool dockToMainViewport = false);

    // Called synchronously after a successful project create/open/close. This
    // lets the application rebuild project-bound GPU/session presentation in
    // the same UI frame before any other panel can observe stale references.
    void SetWorkspaceChangedCallback(
        std::function<void()> callback);

    // The Create Project / Open Project actions as callable operations, so the
    // buttons, RPC and MCP all run one code path (recents, status line and the
    // workspace-changed hand-off included). Both throw on failure and return
    // the manifest path of the project now bound to the browser workspace.
    [[nodiscard]] std::filesystem::path CreateProject(
        const std::filesystem::path& rootDirectory,
        std::string_view displayName);
    [[nodiscard]] std::filesystem::path OpenProject(
        const std::filesystem::path& path);
    [[nodiscard]] std::vector<studio_session::RecentProjectItem>
    RecentProjects() const;

    // Window that owns the native folder pickers. Without one, the path
    // fields remain typeable but the Browse buttons are hidden.
    // The project the running editor is actually bound to. The browser's own
    // workspace is empty while editing (project switches hand off to a fresh
    // Studio), so the editor reports the open project here for display.
    void SetOpenProject(
        std::string name,
        std::filesystem::path manifestPath,
        std::string activeWorld);

    void SetDialogOwner(
        const platform::Window* owner) noexcept;

    inline static constexpr editor_ui::PanelId kProjectBrowserPanel{
        .high = 0x4f52424954535455ULL,
        .low = 0x50524f4a42525753ULL
    };

private:
    void DrawProjectBrowser(editor_ui::PanelContext& context);
    void NotifyWorkspaceChanged();

    studio_session::StudioWorkspace* workspace_{nullptr};
    studio_session::ProjectBrowserModel projectBrowser_;
    std::function<void()> workspaceChanged_;
    const platform::Window* dialogOwner_{nullptr};

    std::string openProjectName_;
    std::filesystem::path openProjectManifest_;
    std::string openProjectWorld_;
    std::string recentFilter_;

    // Projects found by scanning the usual folders (scanned in the background).
    std::future<std::vector<studio_session::DiscoveredProject>>
        discoveryFuture_;
    std::vector<studio_session::DiscoveredProject> discovered_;
    bool discoveryRan_{false};
    std::string newProjectRoot_;
    std::string newProjectName_{"New Orbit Project"};
    std::string openProjectPath_;

    std::string status_;
};
} // namespace orbit::studio_ui
