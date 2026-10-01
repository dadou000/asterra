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

[[nodiscard]] std::string WorldLabel(
    const editor_session::WorldDocumentItem& item)
{
    std::string label;

    if (item.active)
    {
        label += "[Active] ";
    }

    label += item.descriptor.displayName.empty()
        ? item.descriptor.relativePath.stem().string()
        : item.descriptor.displayName;

    if (item.descriptor.startup)
    {
        label += " [Startup]";
    }

    if (!item.valid)
    {
        label += " [Invalid]";
    }

    label += "##world:";
    label += item.descriptor.relativePath.generic_string();
    return label;
}
} // namespace

ProjectAuthoringUi::ProjectAuthoringUi(
    studio_session::StudioWorkspace& workspace,
    std::filesystem::path recentProjectsFile)
    : workspace_(&workspace),
      projectBrowser_(
          workspace,
          std::move(recentProjectsFile)),
      projectSettings_(workspace)
{
    SynchronizeProjectBuffers();
}

void ProjectAuthoringUi::Register(
    editor_ui::EditorUi& ui)
{
    RegisterProjectBrowser(ui);
    RegisterProjectSettings(ui);
    RegisterWorldDocuments(ui, true);
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

void ProjectAuthoringUi::RegisterProjectSettings(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kProjectSettingsPanel,
        .title = "Project Settings",
        .defaultOpen = false,
        .defaultDock = orbit::editor_ui::DockRegion::Right,
        .dockOrder = 30,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawProjectSettings(context);
            }
    });
}

void ProjectAuthoringUi::RegisterWorldDocuments(
    editor_ui::EditorUi& ui,
    const bool allowCloseWorld)
{
    allowCloseWorld_ = allowCloseWorld;

    ui.RegisterPanel({
        .id = kWorldDocumentsPanel,
        .title = "World Documents",
        .defaultOpen = true,
        .defaultDock = orbit::editor_ui::DockRegion::Left,
        .dockOrder = 10,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawWorldDocuments(context);
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
    SynchronizeProjectBuffers();

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
                SynchronizeProjectBuffers();
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

void ProjectAuthoringUi::DrawProjectSettings(
    editor_ui::PanelContext& context)
{
    SynchronizeProjectBuffers();

    if (!workspace_->HasProject())
    {
        context.Text("No project is open.");
        return;
    }

    const auto snapshot = projectSettings_.Snapshot();

    context.Text(
        std::format(
            "Project ID: {}",
            snapshot.projectId.ToString()));
    context.Text(
        std::format(
            "Root: {}",
            snapshot.rootDirectory.generic_string()));
    context.Text(
        std::format(
            "Engine compatibility: {}",
            snapshot.engineCompatibilityVersion));

    static_cast<void>(
        context.InputText(
            "Display Name##project-display-name",
            projectDisplayName_));

    if (context.Button("Save Project Name"))
    {
        try
        {
            projectSettings_.SetDisplayName(
                projectDisplayName_);
            status_ = "Project name saved.";
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
        }
    }

    context.Separator();
    context.Text(
        std::format(
            "Startup world: {}",
            snapshot.startupWorld.generic_string()));

    for (const auto& world : snapshot.worlds)
    {
        context.Text(WorldLabel(world));

        if (!world.valid)
        {
            context.Text(
                std::format(
                    "  Validation: {}",
                    world.diagnostic));
            continue;
        }

        if (!world.descriptor.startup)
        {
            const std::string button =
                "Set Startup##settings:" +
                world.descriptor.id.ToString();

            if (context.Button(button))
            {
                try
                {
                    static_cast<void>(
                        projectSettings_.SetStartupWorld(
                            world.descriptor.relativePath));
                    status_ = "Startup world updated.";
                }
                catch (const std::exception& exception)
                {
                    status_ = exception.what();
                }
            }
        }
    }

    context.Separator();
    context.Text("Terrain Validation");
    context.Text(
        "Developer check: checkpoint, close/reopen the real project/session, "
        "verify authored identity and fresh derived residency, then regenerate "
        "the same physical page.");

    if (context.Button(
            "Save, Reopen & Verify Terrain"))
    {
        try
        {
            terrainRoundTripReport_ =
                studio_session::
                    VerifyStudioTerrainRoundTrip(
                        *workspace_,
                        "studio.primary");

            terrainRoundTripReportGeneration_ =
                workspace_->Generation();

            const auto& report =
                *terrainRoundTripReport_;

            status_ =
                report.success
                    ? "Terrain round trip verified: authored state and regenerated physical result are equivalent."
                    : std::format(
                          "Terrain round trip failed at {}: {}",
                          report.failureStage.empty()
                              ? std::string("unknown")
                              : report.failureStage,
                          report.diagnostic);

            SynchronizeProjectBuffers();
            NotifyWorkspaceChanged();

            // The workspace owns a new StudioSession after this action. End
            // this panel draw immediately so no pre-reopen presentation state
            // can be observed later in the same callback.
            return;
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
            terrainRoundTripReport_.reset();
        }
    }

    if (terrainRoundTripReport_.has_value())
    {
        const auto& report =
            *terrainRoundTripReport_;

        context.Text(
            std::format(
                "Result: {}",
                report.success
                    ? "PASS"
                    : "FAIL"));

        context.Text(
            std::format(
                "Semantic fingerprint: {} -> {}",
                report.semanticFingerprintBefore,
                report.semanticFingerprintAfter));

        context.Text(
            std::format(
                "Terrain source revision: {} -> {}",
                report.terrainSourceRevisionBefore,
                report.terrainSourceRevisionAfter));

        context.Text(
            std::format(
                "Physical/M29 fingerprint: {} -> {}",
                report.physicalFingerprintBefore,
                report.physicalFingerprintAfter));

        context.Text(
            std::format(
                "Derived reset: M26 {} | M29 {} | page {}",
                report.derivedCacheFreshAfterReopen
                    ? "fresh"
                    : "stale",
                report.debugResidencyFreshAfterReopen
                    ? "fresh"
                    : "stale",
                report.comparisonPagePreserved
                    ? "preserved"
                    : "changed"));

        if (!report.diagnostic.empty())
        {
            context.Text(report.diagnostic);
        }
    }

    context.Separator();
    context.Text("M15 End-to-End Terrain Scenario");
    context.Text(
        "Runs the deterministic production-terrain acceptance sequence in an "
        "isolated scratch Studio project. The currently open project is not modified.");

    if (context.Button(
            "Run M15 Terrain Validation Scenario"))
    {
        try
        {
            const auto validationRoot =
                std::filesystem::temp_directory_path() /
                ("orbit-studio-m15-" +
                 documents::ProjectId::Random().
                     ToString());

            terrainValidationScenarioReport_ =
                studio_session::
                    RunStudioTerrainValidationScenario(
                        validationRoot,
                        "studio.primary");

            const auto& report =
                *terrainValidationScenarioReport_;

            status_ =
                report.success
                    ? "M15 terrain validation scenario passed."
                    : std::format(
                          "M15 terrain validation failed at {}: {}",
                          report.failureStage.empty()
                              ? std::string("unknown")
                              : report.failureStage,
                          report.diagnostic);
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
            terrainValidationScenarioReport_.reset();
        }
    }

    if (terrainValidationScenarioReport_.has_value())
    {
        const auto& report =
            *terrainValidationScenarioReport_;

        context.Text(
            std::format(
                "M15 Result: {} | Steps: {}",
                report.success
                    ? "PASS"
                    : "FAIL",
                report.steps.size()));

        context.Text(
            std::format(
                "Scratch project: {}",
                report.projectRoot.generic_string()));

        context.Text(
            std::format(
                "M29 fields: {} | physical LOD {} | biome weights {}",
                report.debugFieldsAvailable,
                report.debugPhysicalLodAvailable
                    ? "yes"
                    : "no",
                report.debugBiomeWeightsAvailable
                    ? "yes"
                    : "no"));

        context.Text(
            std::format(
                "M26 cache: {} pages | {} bytes | hits {} | misses {}",
                report.cacheStats.residentPages,
                report.cacheStats.residentBytes,
                report.cacheStats.hits,
                report.cacheStats.misses));

        for (const auto& step :
             report.steps)
        {
            context.Text(
                std::format(
                    "{} {}{}{}",
                    step.passed
                        ? "[PASS]"
                        : "[FAIL]",
                    step.name,
                    step.diagnostic.empty()
                        ? ""
                        : " | ",
                    step.diagnostic));
        }
    }

    if (!status_.empty())
    {
        context.Separator();
        context.Text(status_);
    }
}

void ProjectAuthoringUi::DrawWorldDocuments(
    editor_ui::PanelContext& context)
{
    SynchronizeProjectBuffers();

    if (!workspace_->HasProject())
    {
        context.Text("No project is open.");
        return;
    }

    context.Text("Create World");
    static_cast<void>(
        context.InputText(
            "Path##create-world-path",
            createWorldPath_));
    static_cast<void>(
        context.InputText(
            "Display Name##create-world-name",
            createWorldName_));

    if (context.Button("Create World"))
    {
        try
        {
            const auto created =
                workspace_->Session().CreateWorld(
                    createWorldPath_,
                    createWorldName_);
            selectedWorld_ = created.relativePath;
            selectedWorldName_ = created.displayName;
            status_ = "World created.";
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
        }
    }

    context.Separator();

    const auto catalog = workspace_->Session().Worlds();

    for (const auto& item : catalog)
    {
        const bool selected =
            selectedWorld_.has_value() &&
            item.descriptor.relativePath == *selectedWorld_;

        if (context.Selectable(
                WorldLabel(item),
                selected))
        {
            selectedWorld_ = item.descriptor.relativePath;
            selectedWorldName_ = item.descriptor.displayName;
        }

        context.Text(
            std::format(
                "  {}",
                item.descriptor.relativePath.generic_string()));

        if (item.valid)
        {
            context.Text(
                std::format(
                    "  ID {} | schema {}",
                    item.descriptor.id.ToString(),
                    item.descriptor.schemaVersion));
        }
        else
        {
            context.Text(
                std::format(
                    "  Validation: {}",
                    item.diagnostic));
        }
    }

    if (selectedWorld_.has_value())
    {
        const auto selected =
            std::find_if(
                catalog.begin(),
                catalog.end(),
                [this](const auto& item)
                {
                    return item.descriptor.relativePath ==
                        *selectedWorld_;
                });

        if (selected == catalog.end())
        {
            selectedWorld_.reset();
            selectedWorldName_.clear();
        }
        else
        {
            context.Separator();
            context.Text(
                std::format(
                    "Selected: {}",
                    selected->descriptor.relativePath.generic_string()));

            if (!selected->valid)
            {
                context.Text(selected->diagnostic);
            }
            else
            {
                if (!selected->active &&
                    context.Button("Open Selected World"))
                {
                    try
                    {
                        workspace_->Session().OpenWorld(
                            selected->descriptor.relativePath);
                        status_ = "World opened.";
                    }
                    catch (const std::exception& exception)
                    {
                        status_ = exception.what();
                    }
                }

                if (!selected->descriptor.startup &&
                    context.Button("Set Selected As Startup"))
                {
                    try
                    {
                        static_cast<void>(
                            workspace_->Session().SetStartupWorld(
                                selected->descriptor.relativePath));
                        status_ = "Startup world updated.";
                    }
                    catch (const std::exception& exception)
                    {
                        status_ = exception.what();
                    }
                }

                static_cast<void>(
                    context.InputText(
                        "Display Name##selected-world-name",
                        selectedWorldName_));

                if (context.Button("Rename Selected World"))
                {
                    try
                    {
                        const auto renamed =
                            workspace_->Session().RenameWorld(
                                selected->descriptor.relativePath,
                                selectedWorldName_);
                        selectedWorldName_ = renamed.displayName;
                        status_ = "World display name updated.";
                    }
                    catch (const std::exception& exception)
                    {
                        status_ = exception.what();
                    }
                }
            }
        }
    }

    if (allowCloseWorld_ &&
        workspace_->Session().ActiveWorld().has_value() &&
        context.Button("Close Active World"))
    {
        try
        {
            workspace_->Session().CloseWorld();
            status_ = "Active world closed.";
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
        }
    }

    if (!status_.empty())
    {
        context.Separator();
        context.Text(status_);
    }
}

void ProjectAuthoringUi::SynchronizeProjectBuffers()
{
    if (workspace_->Generation() == observedWorkspaceGeneration_)
    {
        return;
    }

    observedWorkspaceGeneration_ = workspace_->Generation();

    if (terrainRoundTripReport_.has_value() &&
        terrainRoundTripReportGeneration_ !=
            observedWorkspaceGeneration_)
    {
        terrainRoundTripReport_.reset();
        terrainRoundTripReportGeneration_ =
            ~u64{0};
    }

    selectedWorld_.reset();
    selectedWorldName_.clear();

    if (!workspace_->HasProject())
    {
        projectDisplayName_.clear();
        return;
    }

    projectDisplayName_ =
        workspace_->Project().Manifest().displayName;
}

std::filesystem::path ProjectAuthoringUi::CreateProject(
    const std::filesystem::path& rootDirectory,
    const std::string_view displayName)
{
    projectBrowser_.CreateProject(
        rootDirectory,
        displayName);
    status_ = "Project created and opened.";
    SynchronizeProjectBuffers();
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
    SynchronizeProjectBuffers();
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
