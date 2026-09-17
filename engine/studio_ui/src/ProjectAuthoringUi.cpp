#include <orbit/studio_ui/ProjectAuthoringUi.hpp>

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

namespace orbit::studio_ui
{
namespace
{
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
    ui.RegisterPanel({
        .id = kProjectBrowserPanel,
        .title = "Project Browser",
        .defaultOpen = true,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawProjectBrowser(context);
            }
    });

    ui.RegisterPanel({
        .id = kProjectSettingsPanel,
        .title = "Project Settings",
        .defaultOpen = false,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawProjectSettings(context);
            }
    });

    ui.RegisterPanel({
        .id = kWorldDocumentsPanel,
        .title = "World Documents",
        .defaultOpen = true,
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

void ProjectAuthoringUi::DrawProjectBrowser(
    editor_ui::PanelContext& context)
{
    SynchronizeProjectBuffers();

    if (workspace_->HasProject())
    {
        const auto& project = workspace_->Project();
        context.Text(
            std::format(
                "Open: {}",
                project.Manifest().displayName));
        context.Text(
            project.ManifestPath().generic_string());

        if (context.Button("Close Project"))
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

        context.Separator();
    }

    context.Text("New Project");
    static_cast<void>(
        context.InputText(
            "Root##new-project-root",
            newProjectRoot_));
    static_cast<void>(
        context.InputText(
            "Name##new-project-name",
            newProjectName_));

    if (context.Button("Create Project"))
    {
        try
        {
            projectBrowser_.CreateProject(
                std::filesystem::path(newProjectRoot_),
                newProjectName_);
            status_ = "Project created and opened.";
            SynchronizeProjectBuffers();
            NotifyWorkspaceChanged();
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
        }
    }

    context.Separator();
    context.Text("Open Project");
    static_cast<void>(
        context.InputText(
            "Path##open-project-path",
            openProjectPath_));

    if (context.Button("Open Project"))
    {
        try
        {
            projectBrowser_.OpenProject(
                std::filesystem::path(openProjectPath_));
            status_ = "Project opened.";
            SynchronizeProjectBuffers();
            NotifyWorkspaceChanged();
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
        }
    }

    context.Separator();
    context.Text("Recent Projects");

    for (const auto& recent : projectBrowser_.RecentProjects())
    {
        std::string label =
            recent.displayName.empty()
                ? recent.manifestPath.parent_path().filename().string()
                : recent.displayName;

        if (!recent.available)
        {
            label += " [Unavailable]";
        }

        label += "##recent:";
        label += recent.manifestPath.generic_string();

        if (context.Selectable(label, false))
        {
            if (!recent.available)
            {
                status_ = recent.error;
                continue;
            }

            try
            {
                projectBrowser_.OpenProject(
                    recent.manifestPath);
                status_ = "Recent project opened.";
                SynchronizeProjectBuffers();
                NotifyWorkspaceChanged();
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
            }
        }

        context.Text(recent.manifestPath.generic_string());
    }

    if (!status_.empty())
    {
        context.Separator();
        context.Text(status_);
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

    if (workspace_->Session().ActiveWorld().has_value() &&
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

void ProjectAuthoringUi::NotifyWorkspaceChanged()
{
    if (workspaceChanged_)
    {
        workspaceChanged_();
    }
}
} // namespace orbit::studio_ui
