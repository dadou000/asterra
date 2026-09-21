#include <orbit/studio_ui/CelestialAuthoringUi.hpp>

#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <exception>
#include <format>
#include <string>

namespace orbit::studio_ui
{
CelestialAuthoringUi::CelestialAuthoringUi(
    studio_session::StudioSession& session)
    : session_(session)
{
}

void CelestialAuthoringUi::Register(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanel,
        .title = "Celestial",
        .defaultOpen = true,
        .defaultDock =
            editor_ui::DockRegion::Right,
        .dockOrder = 5,
        .minSize = {
            .width = 280.0F,
            .height = 240.0F
        },
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                Draw(context);
            }
    });
}

void CelestialAuthoringUi::Draw(
    editor_ui::PanelContext& context)
{
    auto& world = session_.World();

    if (!world.HasWorld())
    {
        context.Text(
            "Open a world to author celestial systems.");
        return;
    }

    editor_model::CelestialAuthoringModel model(
        world.Objects(),
        world.Schemas(),
        world.Commands(),
        world.Selection());

    context.Heading("Hierarchy");

    const auto selected =
        model.PrimarySelection();

    if (selected.has_value())
    {
        context.Text(
            std::format(
                "Selected: {}",
                selected->name));
    }
    else
    {
        context.MutedText(
            "Select a system/body/reference node in Explorer.");
    }

    static_cast<void>(
        context.InputText(
            "System Name##celestial-system-name",
            newSystemName_));

    if (context.Button(
            "+ System##celestial-create-system"))
    {
        try
        {
            static_cast<void>(
                model.CreateSystem(
                    newSystemName_));
            status_ =
                "Celestial system created.";
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
        }
    }

    static_cast<void>(
        context.InputText(
            "Body Name##celestial-body-name",
            newBodyName_));

    if (context.Button(
            "+ Body##celestial-create-body"))
    {
        try
        {
            static_cast<void>(
                model.CreateBody(
                    newBodyName_));
            status_ =
                "Celestial body created.";
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
        }
    }

    static_cast<void>(
        context.InputText(
            "Reference Name##celestial-reference-name",
            newReferenceName_));

    if (context.Button(
            "+ Reference / Barycenter##celestial-create-reference"))
    {
        try
        {
            static_cast<void>(
                model.CreateReferenceNode(
                    newReferenceName_));
            status_ =
                "Reference/barycenter node created.";
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
        }
    }

    context.Separator();
    context.Heading("Capabilities");

    const auto body =
        model.SelectedBody();

    if (!body.has_value())
    {
        context.MutedText(
            "Select a body or one of its capability children.");
    }
    else
    {
        context.Text(
            std::format(
                "Target: {}",
                body->name));

        for (const auto& capability :
             model.AvailableCapabilities())
        {
            const std::string label =
                "+ " +
                std::string(
                    capability.label) +
                "##celestial-capability-" +
                capability.type.ToString();

            if (context.Button(label))
            {
                try
                {
                    static_cast<void>(
                        model.AddCapability(
                            capability.type,
                            capability.label));
                    status_ =
                        std::string(
                            capability.label) +
                        " capability added.";
                }
                catch (const std::exception& exception)
                {
                    status_ = exception.what();
                }
            }
        }
    }

    if (selected.has_value() &&
        model.IsCapabilityType(
            selected->type))
    {
        context.Separator();

        if (context.Button(
                "Remove Selected Capability##celestial-remove-capability"))
        {
            try
            {
                model.RemoveSelectedCapability();
                status_ =
                    "Capability removed.";
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
            }
        }
    }

    context.Separator();
    context.Heading("Validation");

    if (context.Button(
            "Validate Celestial Model##celestial-validate"))
    {
        diagnostics_ =
            model.Validate();

        try
        {
            static_cast<void>(
                world.RebuildUniverse());

            if (diagnostics_.size() == 1U &&
                diagnostics_.front().severity ==
                    editor_model::
                        CelestialDiagnosticSeverity::
                            Info)
            {
                status_ =
                    "Semantic and runtime celestial validation passed.";
            }
            else
            {
                status_ =
                    "Semantic validation completed with diagnostics.";
            }
        }
        catch (const std::exception& exception)
        {
            diagnostics_.push_back({
                .severity =
                    editor_model::
                        CelestialDiagnosticSeverity::
                            Error,
                .message =
                    std::string(
                        "Runtime composition: ") +
                    exception.what(),
                .object =
                    std::nullopt
            });

            status_ =
                "Runtime celestial validation failed.";
        }
    }

    if (diagnostics_.empty())
    {
        context.MutedText(
            "Run validation to inspect hierarchy and runtime composition.");
    }
    else
    {
        for (const auto& diagnostic :
             diagnostics_)
        {
            const char* prefix = "[info]";

            if (diagnostic.severity ==
                editor_model::
                    CelestialDiagnosticSeverity::
                        Warning)
            {
                prefix = "[warning]";
            }
            else if (
                diagnostic.severity ==
                editor_model::
                    CelestialDiagnosticSeverity::
                        Error)
            {
                prefix = "[error]";
            }

            context.Text(
                std::format(
                    "{} {}{}",
                    prefix,
                    diagnostic.message,
                    diagnostic.object.
                            has_value()
                        ? " (" +
                              diagnostic.object->
                                  ToString() +
                              ")"
                        : ""));
        }
    }

    if (!status_.empty())
    {
        context.Separator();
        context.Text(status_);
    }
}
} // namespace orbit::studio_ui
