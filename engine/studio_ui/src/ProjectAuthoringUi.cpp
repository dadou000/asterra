#include <orbit/studio_ui/ProjectAuthoringUi.hpp>
#include <orbit/core/ThreadName.hpp>

#include <orbit/platform/Paths.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <system_error>
#include <format>
#include <stdexcept>
#include <utility>

namespace orbit::studio_ui
{
namespace
{
// Paths may contain characters outside the ANSI code page, so UI text built
// from them goes through UTF-8 rather than generic_string().
[[nodiscard]] std::string Utf8(const std::filesystem::path& path)
{
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}
} // namespace

ProjectAuthoringUi::ProjectAuthoringUi(
    studio_session::StudioWorkspace& workspace,
    std::filesystem::path recentProjectsFile)
    : workspace_(&workspace),
      projectBrowser_(
          workspace,
          std::move(recentProjectsFile))
{
}

void ProjectAuthoringUi::RegisterProjectBrowser(
    editor_ui::EditorUi& ui,
    const bool dockToMainViewport)
{
    ui.RegisterPanel({
        .id = kProjectBrowserPanel,
        .title = "Project Browser",
        .defaultOpen = true,
        .dockToMainViewport = dockToMainViewport,
        .defaultDock = orbit::editor_ui::DockRegion::Center,
        .dockOrder = 0,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawProjectBrowser(context);
            }
    });
}

void ProjectAuthoringUi::SetWorkspaceChangedCallback(
    std::function<void()> callback)
{
    workspaceChanged_ = std::move(callback);
}

void ProjectAuthoringUi::SetDialogOwner(
    const platform::Window* const owner) noexcept
{
    dialogOwner_ = owner;
}

void ProjectAuthoringUi::SetOpenProject(
    std::string name,
    std::filesystem::path manifestPath,
    std::string activeWorld)
{
    openProjectName_ = std::move(name);
    openProjectManifest_ = std::move(manifestPath);
    openProjectWorld_ = std::move(activeWorld);
}

void ProjectAuthoringUi::DrawProjectBrowser(
    editor_ui::PanelContext& context)
{
    // A project can be open in the editor (reported by SetOpenProject) or in
    // this panel's own workspace (project-browser start view).
    std::string currentName = openProjectName_;
    std::filesystem::path currentManifest = openProjectManifest_;
    const std::string currentWorld = openProjectWorld_;

    if (currentName.empty() &&
        workspace_->HasProject())
    {
        const auto& project = workspace_->Project();
        currentName = project.Manifest().displayName;
        currentManifest = project.ManifestPath();
    }

    const bool hasCurrent = !currentName.empty();

    context.Heading("ORBIT STUDIO");

    if (hasCurrent)
    {
        context.Heading("Current Project");
        context.Text(currentName);
        context.KeyValue(
            "Location",
            currentManifest.parent_path().generic_string());
        if (!currentWorld.empty())
        {
            context.KeyValue("Active world", currentWorld);
        }
        context.MutedText(
            "Opening another project below switches Studio to it. Your "
            "current project and world are saved first.");

        if (workspace_->HasProject() &&
            context.Button("Close Project"))
        {
            try
            {
                projectBrowser_.CloseProject();
                status_ = "Project closed.";
                NotifyWorkspaceChanged();
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
            }
        }
    }
    else
    {
        context.MutedText(
            "Pick a recent project, open one from disk, or start a new "
            "one. Everything opens straight into the authoring workspace.");
    }

    // Feedback stays next to the actions that produce it.
    if (!status_.empty())
    {
        context.Text(status_);
    }

    const auto browseFolder =
        [&](const char* title,
            std::string& target,
            const char* buttonLabel)
    {
        if (dialogOwner_ == nullptr)
        {
            return;
        }

        context.SameLine();

        if (!context.Button(buttonLabel))
        {
            return;
        }

        try
        {
            if (const auto folder =
                    platform::SelectFolder(
                        *dialogOwner_,
                        {
                            .title = title,
                            .initialDirectory =
                                std::filesystem::path(target)
                        });
                folder.has_value())
            {
                target = folder->string();
            }
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
        }
    };

    const auto openManifest =
        [&](const std::filesystem::path& manifest,
            const char* success)
    {
        try
        {
            static_cast<void>(OpenProject(manifest));
            status_ = success;
        }
        catch (const std::exception& exception)
        {
            status_ = std::format(
                "Could not open {}: {}",
                manifest.generic_string(),
                exception.what());
        }
    };

    const auto recentProjects =
        projectBrowser_.RecentProjects();

    if (context.Section("Recent Projects", true))
    {
        if (recentProjects.empty())
        {
            context.MutedText(
                "No recent projects yet. Create or open one below.");
        }

        if (recentProjects.size() > 6U)
        {
            static_cast<void>(
                context.InputText(
                    "Filter##recent-filter",
                    recentFilter_));
        }

        const auto lower =
            [](std::string text)
        {
            std::ranges::transform(
                text,
                text.begin(),
                [](const unsigned char ch)
                {
                    return static_cast<char>(
                        std::tolower(ch));
                });
            return text;
        };
        const std::string filter = lower(recentFilter_);

        for (const auto& recent : recentProjects)
        {
            const std::string name =
                recent.displayName.empty()
                    ? recent.manifestPath.parent_path().
                          filename().string()
                    : recent.displayName;
            const std::string path =
                recent.manifestPath.parent_path().
                    generic_string();

            if (!filter.empty() &&
                lower(name).find(filter) == std::string::npos &&
                lower(path).find(filter) == std::string::npos)
            {
                continue;
            }

            std::error_code equivalentError;
            const bool isCurrent =
                hasCurrent &&
                std::filesystem::equivalent(
                    recent.manifestPath,
                    currentManifest,
                    equivalentError);

            std::string label = name;
            if (isCurrent)
            {
                label += "   (open)";
            }
            else if (!recent.available)
            {
                label += "   (unavailable)";
            }
            label += "##recent:";
            label += recent.manifestPath.generic_string();

            if (context.Selectable(label, isCurrent) &&
                !isCurrent)
            {
                if (recent.available)
                {
                    openManifest(
                        recent.manifestPath,
                        "Project opened.");
                }
                else
                {
                    status_ = std::format(
                        "{} cannot be opened: {}. Check that the "
                        "folder still exists, or open it again from "
                        "its new location.",
                        name,
                        recent.error.empty()
                            ? std::string("manifest not found")
                            : recent.error);
                }
            }

            context.MutedText(path);
        }
    }

    if (context.Section("Find Projects", true))
    {
        context.MutedText(
            "Scans Documents, Desktop, Downloads and Orbit's Projects folder.");

        if (discoveryFuture_.valid() &&
            discoveryFuture_.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready)
        {
            try
            {
                discovered_ = discoveryFuture_.get();
            }
            catch (const std::exception& exception)
            {
                status_ = std::format(
                    "Project scan failed: {}",
                    exception.what());
            }
        }

        const bool scanning = discoveryFuture_.valid();

        if (scanning)
        {
            context.MutedText("Scanning...");
        }
        else if (context.Button("Scan Usual Folders##project-scan") ||
                 !discoveryRan_)
        {
            discoveryRan_ = true;
            discoveryFuture_ = std::async(
                std::launch::async,
                []
                {
                    core::SetCurrentThreadName("Orbit.ProjectScan");
                    return studio_session::ProjectBrowserModel::
                        DiscoverProjects(
                            platform::UsualProjectFolders());
                });
        }

        if (!scanning && discovered_.empty() && discoveryRan_)
        {
            context.MutedText("No projects found in the usual folders.");
        }

        for (const auto& project : discovered_)
        {
            const bool alreadyRecent = std::ranges::any_of(
                recentProjects,
                [&project](const studio_session::RecentProjectItem& item)
                {
                    std::error_code error;
                    return std::filesystem::equivalent(
                        item.manifestPath,
                        project.manifestPath,
                        error);
                });

            if (alreadyRecent)
            {
                continue;
            }

            std::string label =
                project.displayName.empty()
                    ? Utf8(project.manifestPath.parent_path().filename())
                    : project.displayName;
            label += "##found:";
            label += Utf8(project.manifestPath);

            if (context.Selectable(label, false))
            {
                openManifest(
                    project.manifestPath,
                    "Project opened.");
            }

            context.MutedText(
                Utf8(project.manifestPath.parent_path()));
        }
    }

    if (context.Section("New Project", !hasCurrent))
    {
        if (newProjectRoot_.empty())
        {
            newProjectRoot_ =
                (platform::UserDataDirectory() /
                 "Projects").string();
        }

        static_cast<void>(
            context.InputText(
                "Location##new-project-root",
                newProjectRoot_));
        browseFolder(
            "Choose where to create the project",
            newProjectRoot_,
            "Browse...##new-project-browse");

        static_cast<void>(
            context.InputText(
                "Name##new-project-name",
                newProjectName_));

        context.MutedText(
            std::format(
                "Creates {}",
                (std::filesystem::path(newProjectRoot_) /
                 newProjectName_).generic_string()));

        if (context.PrimaryButton("Create Project"))
        {
            try
            {
                static_cast<void>(
                    CreateProject(
                        std::filesystem::path(newProjectRoot_) /
                            newProjectName_,
                        newProjectName_));
            }
            catch (const std::exception& exception)
            {
                status_ = std::format(
                    "Could not create '{}' in {}: {}",
                    newProjectName_,
                    newProjectRoot_,
                    exception.what());
            }
        }
    }

    if (context.Section("Open From Disk", !hasCurrent))
    {
        context.MutedText(
            "A project folder or its Project.orbit.toml manifest.");

        static_cast<void>(
            context.InputText(
                "Path##open-project-path",
                openProjectPath_));
        browseFolder(
            "Open Orbit project folder",
            openProjectPath_,
            "Browse...##open-project-browse");

        if (context.PrimaryButton("Open Project"))
        {
            openManifest(
                std::filesystem::path(openProjectPath_),
                "Project opened.");
        }
    }
}

std::filesystem::path ProjectAuthoringUi::CreateProject(
    const std::filesystem::path& rootDirectory,
    const std::string_view displayName)
{
    projectBrowser_.CreateProject(
        rootDirectory,
        displayName);
    status_ = "Project created and opened.";
    std::filesystem::path manifest =
        workspace_->Project().ManifestPath();
    NotifyWorkspaceChanged();
    return manifest;
}

std::filesystem::path ProjectAuthoringUi::OpenProject(
    const std::filesystem::path& path)
{
    projectBrowser_.OpenProject(path);
    status_ = "Project opened.";
    std::filesystem::path manifest =
        workspace_->Project().ManifestPath();
    NotifyWorkspaceChanged();
    return manifest;
}

std::vector<studio_session::RecentProjectItem>
ProjectAuthoringUi::RecentProjects() const
{
    return projectBrowser_.RecentProjects();
}

void ProjectAuthoringUi::NotifyWorkspaceChanged()
{
    if (workspaceChanged_)
    {
        workspaceChanged_();
    }
}
} // namespace orbit::studio_ui
