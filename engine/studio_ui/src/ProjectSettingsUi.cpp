#include <orbit/studio_ui/ProjectSettingsUi.hpp>

#include <format>
#include <stdexcept>

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] std::string WorldLabel(
    const editor_session::WorldDocumentItem& item)
{
    std::string label =
        item.descriptor.displayName.empty()
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

    label += "##project-settings-world:";
    label += item.descriptor.relativePath.generic_string();
    return label;
}
} // namespace

ProjectSettingsUi::ProjectSettingsUi(
    documents::ProjectDocument& project,
    studio_session::StudioSession& session)
    : project_(&project),
      session_(&session)
{
    SynchronizeAuthority();
}

void ProjectSettingsUi::Register(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanelId,
        .title = "Project Settings",
        .defaultOpen = false,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                Draw(context);
            }
    });
}

void ProjectSettingsUi::Draw(
    editor_ui::PanelContext& context)
{
    if (project_ == nullptr ||
        session_ == nullptr)
    {
        context.Text("Project settings are unavailable.");
        return;
    }

    SynchronizeAuthority();

    const auto& manifest =
        project_->Manifest();

    context.Text(
        std::format(
            "Project ID: {}",
            manifest.projectId.ToString()));
    context.Text(
        std::format(
            "Root: {}",
            project_->RootDirectory().
                generic_string()));
    context.Text(
        std::format(
            "Manifest: {}",
            project_->ManifestPath().
                generic_string()));
    context.Text(
        std::format(
            "Engine compatibility: {}",
            manifest.engineCompatibilityVersion));

    static_cast<void>(
        context.InputText(
            "Display Name##project-display-name",
            displayName_));

    if (context.Button("Save Project Name"))
    {
        try
        {
            project_->SetDisplayName(
                displayName_);
            observedDisplayName_ =
                project_->Manifest().
                    displayName;
            displayName_ =
                observedDisplayName_;
            status_ =
                "Project name saved.";
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
            manifest.startupWorld.
                generic_string()));

    for (const auto& world :
         session_->Worlds())
    {
        context.Text(
            WorldLabel(world));

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
                "Set Startup##project-settings:" +
                world.descriptor.id.ToString();

            if (context.Button(button))
            {
                try
                {
                    static_cast<void>(
                        session_->SetStartupWorld(
                            world.descriptor.
                                relativePath));
                    status_ =
                        "Startup world updated.";
                }
                catch (const std::exception&
                           exception)
                {
                    status_ =
                        exception.what();
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

void ProjectSettingsUi::SynchronizeAuthority()
{
    if (project_ == nullptr)
    {
        return;
    }

    const std::string& authoritative =
        project_->Manifest().displayName;

    if (authoritative !=
        observedDisplayName_)
    {
        observedDisplayName_ =
            authoritative;
        displayName_ =
            authoritative;
    }
}
} // namespace orbit::studio_ui
