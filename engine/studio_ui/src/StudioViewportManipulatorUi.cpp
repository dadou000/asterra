#include <orbit/studio_ui/StudioViewportManipulatorUi.hpp>

#include <orbit/render_view/RenderView.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace orbit::studio_ui
{
namespace
{
using editor_model::ManipulatorAxis;
using editor_model::ManipulatorSpace;
using editor_model::ManipulatorTool;

constexpr f32 kHandlePixels = 110.0F;
constexpr f32 kHitPixels = 9.0F;
constexpr f32 kRingHitPixels = 7.0F;
constexpr f32 kCenterHitPixels = 11.0F;
constexpr i32 kRingSegments = 64;
constexpr f64 kRingRadiusFraction = 0.85;

constexpr math::Float4 kAxisColors[3]{
    {0.93F, 0.27F, 0.27F, 1.0F},
    {0.40F, 0.80F, 0.30F, 1.0F},
    {0.30F, 0.52F, 0.96F, 1.0F}};
constexpr math::Float4 kHotColor{1.0F, 0.88F, 0.25F, 1.0F};
constexpr math::Float4 kUniformColor{0.92F, 0.92F, 0.92F, 1.0F};
constexpr math::Float4 kLabelColor{0.93F, 0.95F, 0.98F, 1.0F};
constexpr math::Float4 kHintColor{0.70F, 0.74F, 0.80F, 1.0F};

constexpr std::array<ManipulatorAxis, 3> kAxes{
    ManipulatorAxis::X,
    ManipulatorAxis::Y,
    ManipulatorAxis::Z};

[[nodiscard]] std::size_t AxisIndex(
    const ManipulatorAxis axis) noexcept
{
    return axis == ManipulatorAxis::X
        ? 0U
        : axis == ManipulatorAxis::Y ? 1U : 2U;
}

[[nodiscard]] ManipulatorTool ToolOf(
    const GizmoTool tool) noexcept
{
    return tool == GizmoTool::Rotate
        ? ManipulatorTool::Rotate
        : tool == GizmoTool::Scale
            ? ManipulatorTool::Scale
            : ManipulatorTool::Translate;
}

[[nodiscard]] math::Float4 WithAlpha(
    math::Float4 color,
    const f32 alpha) noexcept
{
    color.w = alpha;
    return color;
}

[[nodiscard]] f32 DistanceToSegment(
    const math::Float2 point,
    const math::Float2 a,
    const math::Float2 b) noexcept
{
    const f32 abx = b.x - a.x;
    const f32 aby = b.y - a.y;
    const f32 lengthSquared = abx * abx + aby * aby;
    f32 t = 0.0F;

    if (lengthSquared > 1.0e-6F)
    {
        t = std::clamp(
            ((point.x - a.x) * abx + (point.y - a.y) * aby) /
                lengthSquared,
            0.0F,
            1.0F);
    }

    const f32 dx = point.x - (a.x + abx * t);
    const f32 dy = point.y - (a.y + aby * t);
    return std::sqrt(dx * dx + dy * dy);
}

struct Projected
{
    math::Float2 pixel{};
    bool valid{false};
};

struct ScreenHandle
{
    ManipulatorAxis axis{ManipulatorAxis::X};
    // Translate/Scale: origin -> tip. Rotate: ring polyline points.
    std::vector<Projected> points;
};
} // namespace

struct StudioViewportManipulatorUi::DragState
{
    DragState(
        scene::ObjectStore& objects,
        commands::CommandService& commands)
        : manipulator(objects, commands)
    {
    }

    editor_model::ViewportManipulator manipulator;
    u64 worldGeneration{0U};
    ManipulatorAxis axis{ManipulatorAxis::X};
    scene::ObjectId object{};
    math::Float2 virtualPointer{};
};

StudioViewportManipulatorUi::StudioViewportManipulatorUi() = default;

StudioViewportManipulatorUi::~StudioViewportManipulatorUi()
{
    Cancel();
}

bool StudioViewportManipulatorUi::Dragging() const noexcept
{
    return drag_ != nullptr && drag_->manipulator.Active();
}

void StudioViewportManipulatorUi::Cancel() noexcept
{
    if (drag_ != nullptr)
    {
        drag_->manipulator.Cancel();
        drag_.reset();
    }
}

bool StudioViewportManipulatorUi::Handle(
    editor_ui::PanelContext& context,
    StudioRenderViewSet& views,
    studio_session::StudioSession& session,
    const std::string_view viewId,
    const GizmoSettings& gizmo,
    const math::Float2 relativeMouseDelta)
{
    // The pointer must be read first: it refers to the viewport Image, the
    // item submitted just before this call.
    const auto pointer = context.LastItemPointer();

    const auto abandon =
        [this]
        {
            if (drag_ != nullptr)
            {
                drag_->manipulator.Cancel();
                drag_.reset();
            }
        };

    auto* const view = views.Find(viewId);

    if (view == nullptr ||
        !session.World().HasWorld() ||
        gizmo.tool == GizmoTool::Select)
    {
        abandon();
        return false;
    }

    // A drag survives only while the same single object stays selected in
    // the same authoring world.
    const auto selected = session.World().Selection().Ordered();
    std::optional<scene::ObjectId> selectedObject;

    if (selected.size() == 1U)
    {
        selectedObject = selected.front();
    }

    if (drag_ != nullptr &&
        (!selectedObject.has_value() ||
         *selectedObject != drag_->object ||
         drag_->worldGeneration != session.World().Generation()))
    {
        if (drag_->worldGeneration == session.World().Generation())
        {
            drag_->manipulator.Cancel();
            drag_.reset();
        }
        else
        {
            // The authoring world was replaced; its CommandService is gone,
            // so the manipulator must not touch it again.
            static_cast<void>(drag_.release());
        }
    }

    const f32 width = static_cast<f32>(view->Width());
    const f32 height = static_cast<f32>(view->Height());

    if (!selectedObject.has_value())
    {
        context.OverlayLabelOnLastItem(
            {10.0F, height - 30.0F},
            kHintColor,
            "Select one object to use the transform handles");
        return false;
    }

    const auto target =
        editor_model::ResolveManipulatorTarget(
            session.World().Objects(),
            *selectedObject);
    const ManipulatorTool tool = ToolOf(gizmo.tool);

    if (!target.has_value() ||
        !editor_model::Supports(*target, tool))
    {
        context.OverlayLabelOnLastItem(
            {10.0F, height - 30.0F},
            kHintColor,
            target.has_value()
                ? "This object cannot be rotated or scaled with handles"
                : "This object type has no transform handles yet");
        return false;
    }

    const ManipulatorSpace space =
        gizmo.space == GizmoSpace::Local
            ? ManipulatorSpace::Local
            : ManipulatorSpace::World;

    const render_view::CameraState camera = view->Camera();
    const auto origin =
        render_view::ProjectToViewport(
            camera,
            view->Width(),
            view->Height(),
            target->position);

    if (!origin.has_value())
    {
        return Dragging();
    }

    // Constant on-screen size: the handle length in meters at the object's
    // depth that spans kHandlePixels.
    const f64 tanHalfFov =
        std::tan(
            static_cast<f64>(camera.verticalFovRadians) * 0.5);
    const f64 pixelsPerMeter =
        static_cast<f64>(height) /
        (2.0 * tanHalfFov * origin->depthMeters);

    if (!(pixelsPerMeter > 1.0e-9) || !std::isfinite(pixelsPerMeter))
    {
        return Dragging();
    }

    const f64 handleMeters =
        static_cast<f64>(kHandlePixels * editor_ui::CurrentUiScale()) /
        pixelsPerMeter;
    const math::Float2 originPixel{
        (1.0F - origin->u) * width,
        origin->v * height};

    const auto project =
        [&](const math::Double3& point) -> Projected
    {
        const auto result =
            render_view::ProjectToViewport(
                camera,
                view->Width(),
                view->Height(),
                point);

        if (!result.has_value())
        {
            return {};
        }

        return {
            // Studio shaders use forward x up for screen-right. The shared
            // RenderView projector uses up x forward, so mirror its X here.
            .pixel = {(1.0F - result->u) * width, result->v * height},
            .valid = true};
    };

    // Build the screen-space handles for the current tool.
    std::vector<ScreenHandle> handles;

    for (const ManipulatorAxis axis : kAxes)
    {
        const math::Double3 direction =
            editor_model::ManipulatorAxisDirection(
                *target, tool, space, axis);
        ScreenHandle handle;
        handle.axis = axis;

        if (tool == ManipulatorTool::Rotate)
        {
            const auto axisA =
                kAxes[(AxisIndex(axis) + 1U) % 3U];
            const auto axisB =
                kAxes[(AxisIndex(axis) + 2U) % 3U];
            const math::Double3 b1 =
                editor_model::ManipulatorAxisDirection(
                    *target, tool, space, axisA);
            const math::Double3 b2 =
                editor_model::ManipulatorAxisDirection(
                    *target, tool, space, axisB);
            const f64 radius = handleMeters * kRingRadiusFraction;

            for (i32 i = 0; i <= kRingSegments; ++i)
            {
                const f64 angle =
                    6.283185307179586 *
                    static_cast<f64>(i) /
                    static_cast<f64>(kRingSegments);
                handle.points.push_back(
                    project(
                        target->position +
                        (b1 * std::cos(angle) +
                         b2 * std::sin(angle)) * radius));
            }
        }
        else
        {
            handle.points.push_back(
                {.pixel = originPixel, .valid = true});
            handle.points.push_back(
                project(
                    target->position + direction * handleMeters));
        }

        handles.push_back(std::move(handle));
    }

    // Which handle is under the pointer.
    std::optional<ManipulatorAxis> hovered;
    f32 best = 1.0e9F;

    if (pointer.hovered && !Dragging())
    {
        if (tool == ManipulatorTool::Scale)
        {
            const f32 dx = pointer.position.x - originPixel.x;
            const f32 dy = pointer.position.y - originPixel.y;
            const f32 hit =
                kCenterHitPixels * editor_ui::CurrentUiScale();

            if (std::sqrt(dx * dx + dy * dy) <= hit)
            {
                hovered = ManipulatorAxis::Uniform;
                best = 0.0F;
            }
        }

        const f32 threshold =
            (tool == ManipulatorTool::Rotate
                 ? kRingHitPixels
                 : kHitPixels) *
            editor_ui::CurrentUiScale();

        for (const ScreenHandle& handle : handles)
        {
            for (std::size_t i = 1U; i < handle.points.size(); ++i)
            {
                if (!handle.points[i - 1U].valid ||
                    !handle.points[i].valid)
                {
                    continue;
                }

                const f32 distance =
                    DistanceToSegment(
                        pointer.position,
                        handle.points[i - 1U].pixel,
                        handle.points[i].pixel);

                if (distance <= threshold && distance < best)
                {
                    best = distance;
                    hovered = handle.axis;
                }
            }
        }
    }

    const auto rayAt =
        [&](const math::Float2 position)
        -> std::optional<editor_model::ManipulatorRay>
    {
        const auto ray =
            render_view::ViewportRay(
                camera,
                view->Width(),
                view->Height(),
                1.0F - position.x / std::max(width, 1.0F),
                position.y / std::max(height, 1.0F),
                true);

        if (!ray.has_value())
        {
            return std::nullopt;
        }

        return editor_model::ManipulatorRay{
            ray->origin,
            ray->direction};
    };

    const editor_model::ManipulatorSnap snap{
        .translate = gizmo.translationSnap,
        .translateMeters = gizmo.translationSnapMeters,
        .rotate = gizmo.rotationSnap,
        .rotateDegrees = gizmo.rotationSnapDegrees,
        .scale = gizmo.scaleSnap,
        .scaleStep = gizmo.scaleSnapStep};

    std::string statusText;

    // Start a drag.
    if (hovered.has_value() && pointer.pressed && !Dragging())
    {
        const auto ray = rayAt(pointer.position);

        if (ray.has_value())
        {
            auto candidate =
                std::make_unique<DragState>(
                    session.World().Objects(),
                    session.World().Commands());
            candidate->worldGeneration = session.World().Generation();
            candidate->axis = *hovered;
            candidate->object = *selectedObject;

            try
            {
                if (candidate->manipulator.Begin(
                        *target, tool, space, *hovered, *ray))
                {
                    drag_ = std::move(candidate);
                    drag_->virtualPointer = pointer.position;
                }
                else
                {
                    statusText =
                        "That handle cannot be grabbed from this angle";
                }
            }
            catch (const std::exception& exception)
            {
                statusText = exception.what();
            }
        }
    }

    // Continue, commit or cancel a drag.
    if (Dragging())
    {
        try
        {
            if (pointer.down)
            {
                drag_->virtualPointer.x += relativeMouseDelta.x;
                drag_->virtualPointer.y += relativeMouseDelta.y;
            }

            if (context.KeyPressed(editor_ui::UiKey::Escape))
            {
                drag_->manipulator.Cancel();
                drag_.reset();
                statusText = "Transform cancelled";
            }
            else if (pointer.down)
            {
                const auto ray = rayAt(drag_->virtualPointer);

                if (ray.has_value())
                {
                    drag_->manipulator.Update(*ray, snap);
                }

                statusText = drag_->manipulator.Summary();
            }
            else
            {
                const std::string finished =
                    drag_->manipulator.Summary();
                drag_->manipulator.Commit();
                session.World().
                    AcknowledgeViewportTransformCommit();
                drag_.reset();
                statusText = finished;
            }
        }
        catch (const std::exception& exception)
        {
            drag_.reset();
            statusText = exception.what();
        }
    }

    // Draw.
    std::vector<editor_ui::OverlaySegment> segments;
    std::vector<editor_ui::OverlayDisc> discs;
    const bool dragging = Dragging();
    const ManipulatorAxis activeAxis =
        dragging ? drag_->axis : hovered.value_or(ManipulatorAxis::Uniform);
    const bool anyActive = dragging || hovered.has_value();
    const f32 thickness = 2.5F * editor_ui::CurrentUiScale();

    for (const ScreenHandle& handle : handles)
    {
        const bool hot =
            anyActive && activeAxis == handle.axis;
        const math::Float4 base =
            kAxisColors[AxisIndex(handle.axis)];
        const math::Float4 color =
            hot
                ? kHotColor
                : WithAlpha(base, dragging ? 0.30F : 0.95F);

        for (std::size_t i = 1U; i < handle.points.size(); ++i)
        {
            if (!handle.points[i - 1U].valid ||
                !handle.points[i].valid)
            {
                continue;
            }

            segments.push_back({
                .a = handle.points[i - 1U].pixel,
                .b = handle.points[i].pixel,
                .color = color,
                .thickness = hot ? thickness * 1.6F : thickness});
        }

        if (tool != ManipulatorTool::Rotate &&
            handle.points.size() == 2U &&
            handle.points[1].valid)
        {
            discs.push_back({
                .center = handle.points[1].pixel,
                .radiusPixels =
                    (tool == ManipulatorTool::Translate ? 6.0F : 5.0F) *
                    editor_ui::CurrentUiScale(),
                .color = color,
                .filled = true});
        }
    }

    if (tool == ManipulatorTool::Scale)
    {
        const bool hot =
            anyActive && activeAxis == ManipulatorAxis::Uniform;
        discs.push_back({
            .center = originPixel,
            .radiusPixels = 8.0F * editor_ui::CurrentUiScale(),
            .color = hot ? kHotColor : WithAlpha(kUniformColor, 0.9F),
            .filled = false});
    }
    else
    {
        discs.push_back({
            .center = originPixel,
            .radiusPixels = 3.5F * editor_ui::CurrentUiScale(),
            .color = WithAlpha(kUniformColor, 0.9F),
            .filled = true});
    }

    context.OverlayShapesOnLastItem(segments, discs);

    if (!statusText.empty())
    {
        context.OverlayLabelOnLastItem(
            {10.0F, height - 30.0F},
            kLabelColor,
            statusText);
    }

    return dragging || hovered.has_value();
}
} // namespace orbit::studio_ui
