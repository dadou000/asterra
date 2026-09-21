#include <orbit/studio_ui/VolumeAuthoringUi.hpp>

#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <array>
#include <exception>
#include <format>

namespace orbit::studio_ui
{
VolumeAuthoringUi::VolumeAuthoringUi(
    studio_session::StudioSession& session) noexcept
    : session_(&session)
{
}

void VolumeAuthoringUi::Register(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanel,
        .title = "Volumes",
        .defaultOpen = false,
        .defaultDock =
            editor_ui::DockRegion::Right,
        .dockOrder = 35,
        .minSize = {
            .width = 300.0F,
            .height = 260.0F
        },
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                Draw(context);
            }
    });
}

void VolumeAuthoringUi::Draw(
    editor_ui::PanelContext& context)
{
    if (session_ == nullptr ||
        !session_->World().HasWorld())
    {
        context.MutedText(
            "Open a world to author volumes.");
        return;
    }

    auto& world =
        session_->World();

    context.Heading("Create Volume");
    context.MutedText(
        "Presets author ordinary editable Volume domains. Select a World or Celestial Body first.");

    constexpr std::array presets{
        "Empty",
        "Smoke",
        "Fire",
        "Fog",
        "Dust",
        "Snow",
        "Surface Flow"
    };

    for (std::size_t index = 0U;
         index < presets.size();
         ++index)
    {
        const std::string label =
            std::string(presets[index]) +
            "##volume-preset-" +
            std::to_string(index);

        if (context.Button(label))
        {
            try
            {
                commands::CommandArguments args;
                args.emplace(
                    "preset",
                    std::string(
                        presets[index]));

                world.CommandRegistry().Invoke(
                    editor_model::
                        authoring_commands::
                            kCreateVolume,
                    args);

                status_ =
                    std::string(presets[index]) +
                    " Volume created.";
            }
            catch (const std::exception& exception)
            {
                status_ =
                    exception.what();
            }
        }

        if (index + 1U < presets.size())
        {
            context.SameLine();
        }
    }

    const auto& selected =
        world.Selection().Ordered();

    if (selected.size() == 1U)
    {
        const auto volume =
            world_model::
                ResolveVolumeDomain(
                    world.Objects(),
                    selected.front());

        if (volume.has_value())
        {
            context.Separator();
            context.Heading("Selected Volume");

            context.Text(
                std::format(
                    "{} | {} | fields 0x{:X}",
                    world_model::
                        VolumeSolverPolicyName(
                            volume->solverPolicy),
                    world_model::
                        VolumeRepresentationModeName(
                            volume->representationMode),
                    volume->fieldMask));

            context.Text(
                std::format(
                    "Sources {} | Effectors {} | Base resolution {}",
                    volume->sourceCount,
                    volume->effectorCount,
                    volume->resolution));

            context.MutedText(
                "Exact domain, solver, representation and field properties are edited in the normal Properties Inspector.");

            if (context.Button(
                    "Remove Selected Volume##volume-remove"))
            {
                try
                {
                    world.CommandRegistry().Invoke(
                        editor_model::
                            authoring_commands::
                                kRemoveVolume);
                    status_ =
                        "Volume removed.";
                }
                catch (const std::exception& exception)
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
} // namespace orbit::studio_ui
