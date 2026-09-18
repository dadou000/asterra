#include <orbit/studio_ui/WorldDocumentsUi.hpp>

#include <algorithm>
#include <format>
#include <stdexcept>

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

WorldDocumentsUi::WorldDocumentsUi(
    studio_session::StudioSession& session,
    const bool allowCloseWorld) noexcept
    : session_(&session),
      allowCloseWorld_(allowCloseWorld)
{
}

void WorldDocumentsUi::Register(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanelId,
        .title = "World Documents",
        .defaultOpen = true,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                Draw(context);
            }
    });
}

void WorldDocumentsUi::Draw(
    editor_ui::PanelContext& context)
{
    if (session_ == nullptr)
    {
        context.Text("World session is unavailable.");
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
                session_->CreateWorld(
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

    const auto catalog =
        session_->Worlds();

    for (const auto& item : catalog)
    {
        const bool selected =
            selectedWorld_.has_value() &&
            item.descriptor.relativePath ==
                *selectedWorld_;

        if (context.Selectable(
                WorldLabel(item),
                selected))
        {
            selectedWorld_ =
                item.descriptor.relativePath;
            selectedWorldName_ =
                item.descriptor.displayName;
        }

        context.Text(
            std::format(
                "  {}",
                item.descriptor.relativePath.
                    generic_string()));

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
                    return item.descriptor.
                               relativePath ==
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
                    selected->descriptor.
                        relativePath.generic_string()));

            if (!selected->valid)
            {
                context.Text(
                    selected->diagnostic);
            }
            else
            {
                if (!selected->active &&
                    context.Button(
                        "Open Selected World"))
                {
                    try
                    {
                        session_->OpenWorld(
                            selected->descriptor.
                                relativePath);
                        status_ = "World opened.";
                    }
                    catch (const std::exception&
                               exception)
                    {
                        status_ =
                            exception.what();
                    }
                }

                if (!selected->descriptor.startup &&
                    context.Button(
                        "Set Selected As Startup"))
                {
                    try
                    {
                        static_cast<void>(
                            session_->SetStartupWorld(
                                selected->descriptor.
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

                static_cast<void>(
                    context.InputText(
                        "Display Name##selected-world-name",
                        selectedWorldName_));

                if (context.Button(
                        "Rename Selected World"))
                {
                    try
                    {
                        const auto renamed =
                            session_->RenameWorld(
                                selected->descriptor.
                                    relativePath,
                                selectedWorldName_);
                        selectedWorldName_ =
                            renamed.displayName;
                        status_ =
                            "World display name updated.";
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
    }

    if (allowCloseWorld_ &&
        session_->ActiveWorld().has_value() &&
        context.Button("Close Active World"))
    {
        try
        {
            session_->CloseWorld();
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
} // namespace orbit::studio_ui
