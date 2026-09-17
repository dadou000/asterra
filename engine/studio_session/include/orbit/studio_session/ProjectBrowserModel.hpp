#pragma once

#include <orbit/documents/ProjectManifest.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_session
{
struct RecentProjectItem
{
    std::filesystem::path manifestPath;
    std::string displayName;
    documents::ProjectId projectId{};
    std::filesystem::path startupWorld;
    bool available{false};
    std::string error;
};

// Project-browser application model. The only persisted browser state is an
// MRU list of Project.orbit.toml paths; project metadata remains authoritative
// in ProjectDocument/ProjectManifest and is read from there on demand.
class ProjectBrowserModel
{
public:
    ProjectBrowserModel(
        StudioWorkspace& workspace,
        std::filesystem::path recentProjectsFile,
        std::size_t maximumRecentProjects = 12U);

    void CreateProject(
        const std::filesystem::path& rootDirectory,
        std::string_view displayName);

    // Accepts a project directory or Project.orbit.toml path.
    void OpenProject(
        const std::filesystem::path& path);

    void CloseProject();

    [[nodiscard]] bool HasProject() const noexcept;

    [[nodiscard]] std::vector<RecentProjectItem>
    RecentProjects() const;

    void ForgetRecentProject(
        const std::filesystem::path& path);

    void ClearRecentProjects();

    [[nodiscard]] const std::filesystem::path&
    RecentProjectsFile() const noexcept;

private:
    [[nodiscard]] static std::filesystem::path
    NormalizeManifestPath(
        const std::filesystem::path& path);

    void Load();
    void Persist() const;
    void RecordCurrentProject();
    void RecordManifest(
        const std::filesystem::path& manifestPath);

    StudioWorkspace* workspace_{nullptr};
    std::filesystem::path recentProjectsFile_;
    std::size_t maximumRecentProjects_{12U};
    std::vector<std::filesystem::path> recentManifestPaths_;
};
} // namespace orbit::studio_session
