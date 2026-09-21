#include <orbit/studio_ui/SystemViewUi.hpp>

#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <exception>
#include <format>
#include <string>
#include <variant>

namespace orbit::studio_ui
{
SystemViewUi::SystemViewUi(
    studio_session::StudioSession& session)
    : session_(session)
{
}

void SystemViewUi::Register(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanel,
        .title = "System View",
        .defaultOpen = true,
        .defaultDock = editor_ui::DockRegion::Center,
        .dockOrder = 20,
        .minSize = {.width = 360.0F, .height = 260.0F},
        .draw = [this](editor_ui::PanelContext& context)
        {
            Draw(context);
        }
    });
}

void SystemViewUi::Draw(
    editor_ui::PanelContext& context)
{
    auto& world = session_.World();

    if (!world.HasWorld())
    {
        context.Text("Open a world to inspect a celestial system.");
        selectedSystem_.reset();
        timeFieldsInitialized_ = false;
        return;
    }

    editor_model::SystemViewModel model(
        world.Objects(),
        world.Universe(),
        world.Selection());

    const auto systems = model.Systems();

    if (systems.empty())
    {
        context.Text("No celestial systems are authored in this world.");
        selectedSystem_.reset();
        return;
    }

    if (const auto selectionSystem =
            model.SystemForSelection();
        selectionSystem.has_value())
    {
        selectedSystem_ = selectionSystem->id;
    }

    if (!selectedSystem_.has_value() ||
        std::find_if(
            systems.begin(),
            systems.end(),
            [&](const auto& item)
            {
                return item.id == *selectedSystem_;
            }) == systems.end())
    {
        selectedSystem_ = systems.front().id;
    }

    context.Heading("System");

    for (const auto& system : systems)
    {
        const std::string label =
            system.name +
            "##system-view-system-" +
            system.id.ToString();

        if (context.Selectable(
                label,
                selectedSystem_ == system.id))
        {
            selectedSystem_ = system.id;
        }
    }

    context.Separator();
    context.Heading("Simulation Time");

    auto& clock = session_.Clock();

    if (!timeFieldsInitialized_)
    {
        editedTimeMicroseconds_ =
            clock.Time().microsecondsFromEpoch;
        editedRate_ = clock.Rate();
        timeFieldsInitialized_ = true;
    }

    bool playing = clock.Playing();

    if (context.Checkbox(
            "Playing##system-view-playing",
            playing))
    {
        clock.SetPlaying(playing);
    }

    if (context.InputDouble(
            "Rate (sim s / real s)##system-view-rate",
            editedRate_))
    {
        try
        {
            clock.SetRate(editedRate_);
            status_ = "Simulation rate updated.";
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
        }
    }

    if (!clock.Playing())
    {
        editedTimeMicroseconds_ =
            clock.Time().microsecondsFromEpoch;
    }

    if (context.InputInteger(
            "Time (us from epoch)##system-view-time",
            editedTimeMicroseconds_))
    {
        clock.SetPlaying(false);
        clock.SetTime({
            .microsecondsFromEpoch =
                editedTimeMicroseconds_
        });
        status_ = "Simulation time set.";
    }

    if (context.Button("-1 hour##system-view-minus-hour"))
    {
        clock.SetPlaying(false);
        clock.StepSeconds(-3600.0);
    }
    context.SameLine();
    if (context.Button("-1 min##system-view-minus-minute"))
    {
        clock.SetPlaying(false);
        clock.StepSeconds(-60.0);
    }
    context.SameLine();
    if (context.Button("+1 min##system-view-plus-minute"))
    {
        clock.SetPlaying(false);
        clock.StepSeconds(60.0);
    }
    context.SameLine();
    if (context.Button("+1 hour##system-view-plus-hour"))
    {
        clock.SetPlaying(false);
        clock.StepSeconds(3600.0);
    }

    editedTimeMicroseconds_ =
        clock.Time().microsecondsFromEpoch;

    if (const auto selectedSystemRecord =
            world.Objects().Find(*selectedSystem_);
        selectedSystemRecord.has_value() &&
        context.Button("Reset to System Epoch##system-view-reset-epoch"))
    {
        i64 epoch = 0;

        if (const auto stored =
                world.Objects().GetProperty(
                    selectedSystemRecord->id,
                    world_model::kSystemEpochMicroseconds);
            stored.has_value())
        {
            if (const auto* value =
                    std::get_if<i64>(&*stored);
                value != nullptr)
            {
                epoch = *value;
            }
        }

        clock.SetPlaying(false);
        clock.SetTime({
            .microsecondsFromEpoch = epoch
        });
        editedTimeMicroseconds_ = epoch;
    }

    context.Separator();
    context.Heading("FrameGraph State");

    std::vector<editor_model::SystemViewItem> items;

    try
    {
        items = model.Items(
            *selectedSystem_,
            clock.Time());
    }
    catch (const std::exception& exception)
    {
        status_ = exception.what();
    }

    f64 extent = 0.0;

    for (const auto& item : items)
    {
        extent = std::max(
            extent,
            item.distanceFromSystemOriginMeters);
    }

    context.Text(
        std::format(
            "t = {} us | {} object{} | extent {:.6g} m",
            clock.Time().microsecondsFromEpoch,
            items.size(),
            items.size() == 1U ? "" : "s",
            extent));

    for (const auto& item : items)
    {
        const char* kind =
            item.kind ==
                    editor_model::SystemViewObjectKind::Body
                ? "Body"
                : "Reference";

        const f64 normalizedX =
            extent > 0.0
                ? item.positionMeters.x / extent
                : 0.0;
        const f64 normalizedZ =
            extent > 0.0
                ? item.positionMeters.z / extent
                : 0.0;

        const std::string label =
            std::format(
                "{} [{}] r={:.6g}m xz=({:.4f},{:.4f})##system-item-{}",
                item.name,
                kind,
                item.distanceFromSystemOriginMeters,
                normalizedX,
                normalizedZ,
                item.object.ToString());

        if (context.Selectable(
                label,
                world.Selection().Contains(item.object)))
        {
            model.Select(item.object);
        }
    }

    const auto& selected =
        world.Selection().Ordered();

    if (selected.size() == 1U)
    {
        const auto selectedRecord =
            world.Objects().Find(selected.front());

        if (selectedRecord.has_value() &&
            (selectedRecord->type ==
                 world_model::kCelestialBodyType ||
             selectedRecord->type ==
                 world_model::kCelestialReferenceNodeType))
        {
            context.Separator();
            context.Heading("Orbit Visualization Hook");

            try
            {
                const auto preview =
                    model.SampleTrajectory(
                        selectedRecord->id,
                        *selectedSystem_,
                        clock.Time(),
                        86'400.0,
                        64U);

                if (!preview.empty())
                {
                    context.Text(
                        std::format(
                            "24 h trajectory sample ready: {} points",
                            preview.size()));
                }
            }
            catch (const std::exception& exception)
            {
                context.MutedText(
                    std::string("Trajectory unavailable: ") +
                    exception.what());
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
