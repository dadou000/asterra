#include <orbit/studio_ui/SystemViewUi.hpp>

#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <limits>
#include <string>
#include <variant>
#include <vector>

namespace orbit::studio_ui
{
namespace
{
struct SystemCanvasProjection
{
    math::Double3 centerMeters{};
    f64 halfSpanMeters{1.0};
    editor_ui::UiSize size{};

    [[nodiscard]] math::Float2 Project(
        const math::Double3 point) const noexcept
    {
        const f64 span =
            std::max(halfSpanMeters, 1.0);

        return {
            static_cast<f32>(
                0.5 +
                (point.x - centerMeters.x) /
                    (2.0 * span)),
            static_cast<f32>(
                0.5 -
                (point.y - centerMeters.y) /
                    (2.0 * span))
        };
    }

    [[nodiscard]] math::Double3 Unproject(
        const math::Float2 normalized,
        const f64 zMeters) const noexcept
    {
        return {
            centerMeters.x +
                (static_cast<f64>(normalized.x) - 0.5) *
                    (2.0 * halfSpanMeters),
            centerMeters.y -
                (static_cast<f64>(normalized.y) - 0.5) *
                    (2.0 * halfSpanMeters),
            zMeters
        };
    }
};

[[nodiscard]] f64 PixelDistance(
    const math::Float2 a,
    const math::Float2 b,
    const editor_ui::UiSize size) noexcept
{
    const f64 dx =
        static_cast<f64>(a.x - b.x) *
        static_cast<f64>(size.width);
    const f64 dy =
        static_cast<f64>(a.y - b.y) *
        static_cast<f64>(size.height);

    return std::sqrt(dx * dx + dy * dy);
}

[[nodiscard]] const editor_model::SystemViewItem*
FindItem(
    const std::vector<editor_model::SystemViewItem>& items,
    const scene::ObjectId object) noexcept
{
    const auto found =
        std::find_if(
            items.begin(),
            items.end(),
            [object](const auto& item)
            {
                return item.object == object;
            });

    return found == items.end()
        ? nullptr
        : &*found;
}

[[nodiscard]] math::Double3 ParentPosition(
    scene::ObjectStore& objects,
    const std::vector<editor_model::SystemViewItem>& items,
    const scene::ObjectId system,
    const scene::ObjectId object)
{
    const auto record =
        objects.Find(object);

    if (!record.has_value() ||
        !record->parent.has_value() ||
        *record->parent == system)
    {
        return {};
    }

    if (const auto* parent =
            FindItem(
                items,
                *record->parent);
        parent != nullptr)
    {
        return parent->positionMeters;
    }

    return {};
}

[[nodiscard]] bool InsideCanvas(
    const math::Float2 point) noexcept
{
    return point.x >= 0.0F &&
           point.x <= 1.0F &&
           point.y >= 0.0F &&
           point.y <= 1.0F;
}
} // namespace

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
        .minSize = {.width = 460.0F, .height = 360.0F},
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
        context.Text(
            "Open a world to inspect a celestial system.");
        selectedSystem_.reset();
        focusedObject_.reset();
        orbitDraft_.reset();
        referenceDraft_.reset();
        activeGizmo_ = GizmoDragKind::None;
        timeFieldsInitialized_ = false;
        return;
    }

    editor_model::SystemViewModel model(
        world.Objects(),
        world.Universe(),
        world.Selection(),
        world.Commands());

    const auto systems =
        model.Systems();

    if (systems.empty())
    {
        context.Text(
            "No celestial systems are authored in this world.");
        selectedSystem_.reset();
        return;
    }

    if (const auto selectionSystem =
            model.SystemForSelection();
        selectionSystem.has_value())
    {
        selectedSystem_ =
            selectionSystem->id;
    }

    if (!selectedSystem_.has_value() ||
        std::find_if(
            systems.begin(),
            systems.end(),
            [&](const auto& item)
            {
                return item.id ==
                    *selectedSystem_;
            }) == systems.end())
    {
        selectedSystem_ =
            systems.front().id;
        focusedObject_.reset();
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
                selectedSystem_ ==
                    system.id))
        {
            selectedSystem_ =
                system.id;
            focusedObject_.reset();
            viewZoom_ = 1.0;
        }
    }

    auto& clock =
        session_.Clock();

    if (!timeFieldsInitialized_)
    {
        editedTimeMicroseconds_ =
            clock.Time().
                microsecondsFromEpoch;
        editedRate_ =
            clock.Rate();
        timeFieldsInitialized_ = true;
    }

    context.Separator();
    context.Heading("Time");

    bool playing =
        clock.Playing();

    if (context.Checkbox(
            "Playing##system-view-playing",
            playing))
    {
        clock.SetPlaying(
            playing);
    }

    if (context.InputDouble(
            "Rate##system-view-rate",
            editedRate_))
    {
        try
        {
            clock.SetRate(
                editedRate_);
            status_ =
                "Simulation rate updated.";
        }
        catch (const std::exception& exception)
        {
            status_ =
                exception.what();
        }
    }

    if (!clock.Playing())
    {
        editedTimeMicroseconds_ =
            clock.Time().
                microsecondsFromEpoch;
    }

    if (context.InputInteger(
            "Time (us)##system-view-time",
            editedTimeMicroseconds_))
    {
        clock.SetPlaying(false);
        clock.SetTime({
            .microsecondsFromEpoch =
                editedTimeMicroseconds_
        });
        status_ =
            "Simulation time set.";
    }

    if (context.Button(
            "-1 h##system-view-minus-hour"))
    {
        clock.SetPlaying(false);
        clock.StepSeconds(-3600.0);
    }
    context.SameLine();
    if (context.Button(
            "-1 min##system-view-minus-minute"))
    {
        clock.SetPlaying(false);
        clock.StepSeconds(-60.0);
    }
    context.SameLine();
    if (context.Button(
            "+1 min##system-view-plus-minute"))
    {
        clock.SetPlaying(false);
        clock.StepSeconds(60.0);
    }
    context.SameLine();
    if (context.Button(
            "+1 h##system-view-plus-hour"))
    {
        clock.SetPlaying(false);
        clock.StepSeconds(3600.0);
    }

    editedTimeMicroseconds_ =
        clock.Time().
            microsecondsFromEpoch;

    if (const auto selectedSystemRecord =
            world.Objects().Find(
                *selectedSystem_);
        selectedSystemRecord.has_value() &&
        context.Button(
            "System Epoch##system-view-reset-epoch"))
    {
        i64 epoch = 0;

        if (const auto stored =
                world.Objects().
                    GetProperty(
                        selectedSystemRecord->id,
                        world_model::
                            kSystemEpochMicroseconds);
            stored.has_value())
        {
            if (const auto* value =
                    std::get_if<i64>(
                        &*stored);
                value != nullptr)
            {
                epoch = *value;
            }
        }

        clock.SetPlaying(false);
        clock.SetTime({
            .microsecondsFromEpoch =
                epoch
        });
        editedTimeMicroseconds_ =
            epoch;
    }

    std::vector<editor_model::SystemViewItem>
        items;

    try
    {
        items =
            model.Items(
                *selectedSystem_,
                clock.Time());
    }
    catch (const std::exception& exception)
    {
        status_ =
            exception.what();
    }

    if (focusedObject_.has_value() &&
        FindItem(
            items,
            *focusedObject_) == nullptr)
    {
        focusedObject_.reset();
    }

    if (context.Button(
            "Frame All##system-view-frame-all"))
    {
        focusedObject_.reset();
        viewZoom_ = 1.0;
    }
    context.SameLine();
    if (context.Button(
            "Zoom +##system-view-zoom-in"))
    {
        viewZoom_ =
            std::min(
                viewZoom_ * 1.5,
                1.0e6);
    }
    context.SameLine();
    if (context.Button(
            "Zoom -##system-view-zoom-out"))
    {
        viewZoom_ =
            std::max(
                viewZoom_ / 1.5,
                1.0e-3);
    }

    math::Double3 viewCenter{};

    if (focusedObject_.has_value())
    {
        if (const auto* focused =
                FindItem(
                    items,
                    *focusedObject_);
            focused != nullptr)
        {
            viewCenter =
                focused->positionMeters;
        }
    }

    f64 baseExtent = 1.0;

    for (const auto& item : items)
    {
        baseExtent =
            std::max(
                baseExtent,
                math::Length(
                    item.positionMeters -
                    viewCenter));
    }

    const f64 halfSpan =
        std::max(
            baseExtent * 1.12 /
                std::max(viewZoom_, 1.0e-6),
            1.0);

    const auto available =
        context.ContentAvailable();

    const editor_ui::UiSize canvasSize{
        .width =
            std::max(
                available.width,
                320.0F),
        .height =
            std::clamp(
                available.width * 0.62F,
                280.0F,
                620.0F)
    };

    const SystemCanvasProjection projection{
        .centerMeters = viewCenter,
        .halfSpanMeters = halfSpan,
        .size = canvasSize
    };

    context.Separator();
    context.Heading("Orbit Gizmos");

    const auto interaction =
        context.Canvas(
            "System Orbit Canvas##system-orbit-canvas",
            canvasSize);

    const math::Float2 systemOrigin =
        projection.Project({});

    if (InsideCanvas(systemOrigin))
    {
        context.CanvasLine(
            {0.0F, systemOrigin.y},
            {1.0F, systemOrigin.y},
            {0.13F, 0.20F, 0.26F, 0.65F},
            1.0F);
        context.CanvasLine(
            {systemOrigin.x, 0.0F},
            {systemOrigin.x, 1.0F},
            {0.13F, 0.20F, 0.26F, 0.65F},
            1.0F);
        context.CanvasCircle(
            systemOrigin,
            3.0F,
            {0.55F, 0.66F, 0.74F, 0.9F});
    }

    const auto& selected =
        world.Selection().Ordered();

    std::optional<scene::ObjectId>
        selectedObject;

    if (selected.size() == 1U)
    {
        selectedObject =
            selected.front();
    }

    struct SelectedTrajectoryState
    {
        std::vector<
            editor_model::
                SystemViewTrajectorySample>
            samples;
        math::Double3 parentPosition{};
        std::optional<
            editor_model::
                OrbitManipulationTarget>
            target;
        std::optional<math::Float2>
            periapsisScreen;
        std::optional<math::Float2>
            apoapsisScreen;
        f64 periapsisRadius{0.0};
        f64 apoapsisRadius{0.0};
    };

    SelectedTrajectoryState selectedTrajectory{};

    for (const auto& item : items)
    {
        const auto record =
            world.Objects().Find(
                item.object);

        if (!record.has_value())
        {
            continue;
        }

        const math::Double3 parentPosition =
            ParentPosition(
                world.Objects(),
                items,
                *selectedSystem_,
                item.object);

        if (record->parent.has_value())
        {
            const math::Float2 from =
                projection.Project(
                    parentPosition);
            const math::Float2 to =
                projection.Project(
                    item.positionMeters);

            if (InsideCanvas(from) ||
                InsideCanvas(to))
            {
                context.CanvasLine(
                    from,
                    to,
                    item.kind ==
                            editor_model::
                                SystemViewObjectKind::
                                    ReferenceNode
                        ? math::Float4{
                              0.32F,
                              0.55F,
                              0.72F,
                              0.45F}
                        : math::Float4{
                              0.18F,
                              0.28F,
                              0.35F,
                              0.30F},
                    1.0F);
            }
        }

        if (item.kind ==
            editor_model::
                SystemViewObjectKind::Body)
        {
            try
            {
                const f64 duration =
                    model.
                        SuggestedTrajectoryDurationSeconds(
                            item.object);

                const auto trajectory =
                    model.SampleTrajectory(
                        item.object,
                        *selectedSystem_,
                        clock.Time(),
                        duration,
                        128U);

                const bool isSelected =
                    selectedObject.has_value() &&
                    *selectedObject ==
                        item.object;

                for (std::size_t index = 1;
                     index <
                         trajectory.size();
                     ++index)
                {
                    const auto a =
                        projection.Project(
                            trajectory[
                                index - 1U].
                                positionMeters);
                    const auto b =
                        projection.Project(
                            trajectory[index].
                                positionMeters);

                    if (InsideCanvas(a) ||
                        InsideCanvas(b))
                    {
                        context.CanvasLine(
                            a,
                            b,
                            isSelected
                                ? math::Float4{
                                      0.25F,
                                      0.72F,
                                      1.0F,
                                      0.95F}
                                : math::Float4{
                                      0.20F,
                                      0.42F,
                                      0.58F,
                                      0.42F},
                            isSelected
                                ? 2.0F
                                : 1.0F);
                    }
                }

                if (isSelected)
                {
                    selectedTrajectory.samples =
                        trajectory;
                    selectedTrajectory.parentPosition =
                        parentPosition;
                    selectedTrajectory.target =
                        model.AnalyticOrbitTarget(
                            item.object);
                }
            }
            catch (const std::exception&)
            {
                // Open trajectories and imported ephemerides may not cover
                // the default visualization window. The current state marker
                // remains authoritative even when a preview path is absent.
            }
        }
    }

    if (selectedTrajectory.target.has_value() &&
        selectedTrajectory.target->
            values.eccentricity <
            1.0 - 1.0e-10 &&
        !selectedTrajectory.samples.empty())
    {
        f64 minimumRadius =
            std::numeric_limits<f64>::max();
        f64 maximumRadius = 0.0;
        math::Double3 minimumPosition{};
        math::Double3 maximumPosition{};

        for (const auto& sample :
             selectedTrajectory.samples)
        {
            const f64 radius =
                math::Length(
                    sample.positionMeters -
                    selectedTrajectory.
                        parentPosition);

            if (radius < minimumRadius)
            {
                minimumRadius = radius;
                minimumPosition =
                    sample.positionMeters;
            }

            if (radius > maximumRadius)
            {
                maximumRadius = radius;
                maximumPosition =
                    sample.positionMeters;
            }
        }

        selectedTrajectory.periapsisRadius =
            minimumRadius;
        selectedTrajectory.apoapsisRadius =
            maximumRadius;
        selectedTrajectory.periapsisScreen =
            projection.Project(
                minimumPosition);
        selectedTrajectory.apoapsisScreen =
            projection.Project(
                maximumPosition);

        context.CanvasCircle(
            *selectedTrajectory.periapsisScreen,
            7.0F,
            {0.96F, 0.55F, 0.25F, 1.0F});
        context.CanvasCircle(
            *selectedTrajectory.apoapsisScreen,
            7.0F,
            {0.35F, 0.92F, 0.62F, 1.0F});

        context.CanvasText(
            {
                selectedTrajectory.
                    periapsisScreen->x +
                    0.008F,
                selectedTrajectory.
                    periapsisScreen->y
            },
            {0.96F, 0.70F, 0.50F, 1.0F},
            "P");

        context.CanvasText(
            {
                selectedTrajectory.
                    apoapsisScreen->x +
                    0.008F,
                selectedTrajectory.
                    apoapsisScreen->y
            },
            {0.55F, 0.95F, 0.72F, 1.0F},
            "A");
    }

    for (const auto& item : items)
    {
        const auto point =
            projection.Project(
                item.positionMeters);

        if (!InsideCanvas(point))
        {
            continue;
        }

        const bool isSelected =
            selectedObject.has_value() &&
            *selectedObject ==
                item.object;

        const bool isFocused =
            focusedObject_.has_value() &&
            *focusedObject_ ==
                item.object;

        context.CanvasCircle(
            point,
            item.kind ==
                    editor_model::
                        SystemViewObjectKind::
                            Body
                ? 5.0F
                : 4.0F,
            item.kind ==
                    editor_model::
                        SystemViewObjectKind::
                            Body
                ? math::Float4{
                      0.80F,
                      0.88F,
                      0.94F,
                      1.0F}
                : math::Float4{
                      0.32F,
                      0.68F,
                      0.95F,
                      1.0F});

        if (isSelected ||
            isFocused)
        {
            context.CanvasCircle(
                point,
                isFocused
                    ? 10.0F
                    : 8.0F,
                isFocused
                    ? math::Float4{
                          1.0F,
                          0.78F,
                          0.30F,
                          0.95F}
                    : math::Float4{
                          0.25F,
                          0.72F,
                          1.0F,
                          0.95F},
                false,
                2.0F);
        }

        if (isSelected)
        {
            context.CanvasText(
                {
                    point.x + 0.010F,
                    point.y + 0.010F
                },
                {0.88F, 0.93F, 0.97F, 1.0F},
                item.name);
        }
    }

    const math::Float2 pointer{
        interaction.u,
        interaction.v
    };

    bool consumedClick = false;

    if (interaction.clicked &&
        selectedTrajectory.target.has_value())
    {
        const f64 periDistance =
            selectedTrajectory.
                    periapsisScreen.
                    has_value()
                ? PixelDistance(
                      pointer,
                      *selectedTrajectory.
                           periapsisScreen,
                      canvasSize)
                : std::numeric_limits<f64>::max();

        const f64 apoDistance =
            selectedTrajectory.
                    apoapsisScreen.
                    has_value()
                ? PixelDistance(
                      pointer,
                      *selectedTrajectory.
                           apoapsisScreen,
                      canvasSize)
                : std::numeric_limits<f64>::max();

        if (periDistance <= 12.0 ||
            apoDistance <= 12.0)
        {
            activeGizmo_ =
                periDistance <= apoDistance
                    ? GizmoDragKind::Periapsis
                    : GizmoDragKind::Apoapsis;

            orbitDraft_ =
                selectedTrajectory.target;

            const auto parentScreen =
                projection.Project(
                    selectedTrajectory.
                        parentPosition);

            const auto handleScreen =
                activeGizmo_ ==
                        GizmoDragKind::Periapsis
                    ? *selectedTrajectory.
                           periapsisScreen
                    : *selectedTrajectory.
                           apoapsisScreen;

            dragBaseScreenRadiusPixels_ =
                std::max(
                    PixelDistance(
                        parentScreen,
                        handleScreen,
                        canvasSize),
                    1.0);

            dragBasePhysicalRadius_ =
                activeGizmo_ ==
                        GizmoDragKind::Periapsis
                    ? selectedTrajectory.
                          periapsisRadius
                    : selectedTrajectory.
                          apoapsisRadius;

            consumedClick = true;
            clock.SetPlaying(false);
        }
    }

    if (interaction.clicked &&
        !consumedClick &&
        selectedObject.has_value())
    {
        const auto record =
            world.Objects().Find(
                *selectedObject);

        if (record.has_value() &&
            record->type ==
                world_model::
                    kCelestialReferenceNodeType)
        {
            if (const auto* selectedItem =
                    FindItem(
                        items,
                        *selectedObject);
                selectedItem != nullptr &&
                PixelDistance(
                    pointer,
                    projection.Project(
                        selectedItem->
                            positionMeters),
                    canvasSize) <= 12.0)
            {
                activeGizmo_ =
                    GizmoDragKind::
                        ReferencePosition;

                const auto stored =
                    world.Objects().
                        GetProperty(
                            selectedItem->object,
                            world_model::
                                kReferenceNodePositionMeters);

                referenceDraft_ =
                    stored.has_value() &&
                            std::holds_alternative<
                                math::Double3>(
                                    *stored)
                        ? std::get<
                              math::Double3>(
                                  *stored)
                        : math::Double3{};

                consumedClick = true;
                clock.SetPlaying(false);
            }
        }
    }

    if ((activeGizmo_ ==
             GizmoDragKind::Periapsis ||
         activeGizmo_ ==
             GizmoDragKind::Apoapsis) &&
        orbitDraft_.has_value() &&
        interaction.leftDown &&
        interaction.hovered)
    {
        const auto parentScreen =
            projection.Project(
                selectedTrajectory.
                    parentPosition);

        const f64 cursorRadiusPixels =
            PixelDistance(
                pointer,
                parentScreen,
                canvasSize);

        const f64 newRadius =
            std::max(
                dragBasePhysicalRadius_ *
                    cursorRadiusPixels /
                    std::max(
                        dragBaseScreenRadiusPixels_,
                        1.0),
                1.0e-6);

        auto& values =
            orbitDraft_->values;

        f64 periapsis =
            values.semiMajorAxisMeters *
            (1.0 - values.eccentricity);
        f64 apoapsis =
            values.semiMajorAxisMeters *
            (1.0 + values.eccentricity);

        if (activeGizmo_ ==
            GizmoDragKind::Periapsis)
        {
            periapsis =
                std::min(
                    newRadius,
                    apoapsis *
                        (1.0 - 1.0e-9));
        }
        else
        {
            apoapsis =
                std::max(
                    newRadius,
                    periapsis *
                        (1.0 + 1.0e-9));
        }

        values.semiMajorAxisMeters =
            0.5 *
            (periapsis + apoapsis);
        values.eccentricity =
            (apoapsis - periapsis) /
            (apoapsis + periapsis);
        values.periapsisDistanceMeters =
            periapsis;
    }

    if (activeGizmo_ ==
            GizmoDragKind::
                ReferencePosition &&
        referenceDraft_.has_value() &&
        selectedObject.has_value() &&
        interaction.leftDown &&
        interaction.hovered)
    {
        const auto record =
            world.Objects().Find(
                *selectedObject);

        const math::Double3 parentSystem =
            ParentPosition(
                world.Objects(),
                items,
                *selectedSystem_,
                *selectedObject);

        const math::Double3 systemPoint =
            projection.Unproject(
                pointer,
                parentSystem.z +
                    referenceDraft_->z);

        referenceDraft_->x =
            systemPoint.x -
            parentSystem.x;
        referenceDraft_->y =
            systemPoint.y -
            parentSystem.y;
    }

    if (interaction.leftReleased &&
        activeGizmo_ !=
            GizmoDragKind::None)
    {
        try
        {
            if ((activeGizmo_ ==
                     GizmoDragKind::Periapsis ||
                 activeGizmo_ ==
                     GizmoDragKind::Apoapsis) &&
                orbitDraft_.has_value())
            {
                model.ApplyAnalyticOrbitEdit(
                    orbitDraft_->
                        capability,
                    orbitDraft_->
                        values);
                status_ =
                    "Orbit gizmo edit committed.";
            }
            else if (
                activeGizmo_ ==
                    GizmoDragKind::
                        ReferencePosition &&
                selectedObject.has_value() &&
                referenceDraft_.has_value())
            {
                model.SetReferenceNodePosition(
                    *selectedObject,
                    *referenceDraft_);
                status_ =
                    "Reference-node position committed.";
            }
        }
        catch (const std::exception& exception)
        {
            status_ =
                exception.what();
        }

        activeGizmo_ =
            GizmoDragKind::None;
        referenceDraft_.reset();
    }

    if (interaction.clicked &&
        !consumedClick &&
        activeGizmo_ ==
            GizmoDragKind::None)
    {
        const editor_model::SystemViewItem*
            nearest = nullptr;
        f64 nearestPixels =
            std::numeric_limits<f64>::max();

        for (const auto& item : items)
        {
            const auto screen =
                projection.Project(
                    item.positionMeters);

            const f64 distance =
                PixelDistance(
                    pointer,
                    screen,
                    canvasSize);

            if (distance <
                nearestPixels)
            {
                nearestPixels =
                    distance;
                nearest =
                    &item;
            }
        }

        if (nearest != nullptr &&
            nearestPixels <= 14.0)
        {
            model.Select(
                nearest->object);

            if (interaction.doubleClicked)
            {
                focusedObject_ =
                    nearest->object;
                viewZoom_ =
                    std::max(
                        viewZoom_,
                        2.0);
            }
        }
    }

    context.MutedText(
        "Click selects. Double-click focuses. Drag P/A handles to edit periapsis/apapsis. Drag a selected barycenter/reference marker to reposition it.");

    const auto refreshedSelection =
        world.Selection().Ordered();

    if (refreshedSelection.size() == 1U)
    {
        const auto selectedRecord =
            world.Objects().Find(
                refreshedSelection.front());

        if (selectedRecord.has_value() &&
            selectedRecord->type ==
                world_model::
                    kCelestialBodyType)
        {
            const auto target =
                model.AnalyticOrbitTarget(
                    selectedRecord->id);

            if (target.has_value())
            {
                if (!orbitDraft_.has_value() ||
                    orbitDraft_->capability !=
                        target->capability)
                {
                    orbitDraft_ =
                        target;
                }

                context.Separator();
                context.Heading(
                    "Analytic Orbit Manipulator");

                auto& edit =
                    orbitDraft_->values;

                static_cast<void>(
                    context.InputDouble(
                        "Semi-major axis (m)##orbit-gizmo-a",
                        edit.semiMajorAxisMeters));
                static_cast<void>(
                    context.InputDouble(
                        "Periapsis (m)##orbit-gizmo-q",
                        edit.periapsisDistanceMeters));
                static_cast<void>(
                    context.InputDouble(
                        "Eccentricity##orbit-gizmo-e",
                        edit.eccentricity));
                static_cast<void>(
                    context.InputDouble(
                        "Inclination (deg)##orbit-gizmo-i",
                        edit.inclinationDegrees));
                static_cast<void>(
                    context.InputDouble(
                        "Ascending node (deg)##orbit-gizmo-node",
                        edit.ascendingNodeDegrees));
                static_cast<void>(
                    context.InputDouble(
                        "Argument periapsis (deg)##orbit-gizmo-arg",
                        edit.argumentPeriapsisDegrees));
                static_cast<void>(
                    context.InputDouble(
                        "Mean anomaly epoch (deg)##orbit-gizmo-phase",
                        edit.meanAnomalyEpochDegrees));
                static_cast<void>(
                    context.InputDouble(
                        "Mu (m3/s2)##orbit-gizmo-mu",
                        edit.gravitationalParameterM3PerS2));

                if (context.PrimaryButton(
                        "Apply Orbit Edit##orbit-gizmo-apply"))
                {
                    try
                    {
                        model.ApplyAnalyticOrbitEdit(
                            orbitDraft_->
                                capability,
                            edit);
                        status_ =
                            "Analytic orbit edit committed.";
                    }
                    catch (const std::exception& exception)
                    {
                        status_ =
                            exception.what();
                    }
                }

                if (context.Button(
                        "Reset Orbit Draft##orbit-gizmo-reset"))
                {
                    orbitDraft_ =
                        target;
                }
            }
            else
            {
                orbitDraft_.reset();
            }
        }
        else if (
            selectedRecord.has_value() &&
            selectedRecord->type ==
                world_model::
                    kCelestialReferenceNodeType)
        {
            context.Separator();
            context.Heading(
                "Reference Node Manipulator");

            if (!referenceDraft_.has_value())
            {
                const auto stored =
                    world.Objects().
                        GetProperty(
                            selectedRecord->id,
                            world_model::
                                kReferenceNodePositionMeters);

                referenceDraft_ =
                    stored.has_value() &&
                            std::holds_alternative<
                                math::Double3>(
                                    *stored)
                        ? std::get<
                              math::Double3>(
                                  *stored)
                        : math::Double3{};
            }

            static_cast<void>(
                context.InputDouble3(
                    "Parent-frame Position (m)##reference-gizmo-position",
                    *referenceDraft_));

            if (context.PrimaryButton(
                    "Apply Reference Position##reference-gizmo-apply"))
            {
                try
                {
                    model.SetReferenceNodePosition(
                        selectedRecord->id,
                        *referenceDraft_);
                    status_ =
                        "Reference-node position committed.";
                }
                catch (const std::exception& exception)
                {
                    status_ =
                        exception.what();
                }
            }
        }
        else
        {
            orbitDraft_.reset();
            referenceDraft_.reset();
        }
    }
    else
    {
        orbitDraft_.reset();
        referenceDraft_.reset();
    }

    context.Separator();
    context.Text(
        std::format(
            "t={} us | {} object{} | zoom {:.3g}x{}",
            clock.Time().
                microsecondsFromEpoch,
            items.size(),
            items.size() == 1U ? "" : "s",
            viewZoom_,
            focusedObject_.has_value()
                ? " | focused"
                : ""));

    if (!status_.empty())
    {
        context.Text(status_);
    }
}
} // namespace orbit::studio_ui
