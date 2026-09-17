#include <orbit/studio_ui/StudioViewportPanels.hpp>

#include <orbit/paths/PathNetwork.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] const char* ModeName(
    const studio_session::ViewportMode mode) noexcept
{
    switch (mode)
    {
    case studio_session::ViewportMode::Perspective:
        return "Perspective";
    case studio_session::ViewportMode::BodyMap:
        return "Body Map";
    case studio_session::ViewportMode::Debug:
        return "Debug";
    }

    return "Perspective";
}

[[nodiscard]] std::optional<paths::PathEdgeRecord>
SelectedBezierEdge(
    studio_session::StudioSession& session)
{
    if (!session.World().HasWorld())
    {
        return std::nullopt;
    }

    auto& paths = session.PathNetwork().Service();

    for (const auto object :
         session.World().Selection().Ordered())
    {
        const auto edge = paths.FindEdge(object);

        if (edge.has_value() &&
            edge->mode == paths::EdgeMode::Bezier)
        {
            return edge;
        }
    }

    return std::nullopt;
}
} // namespace

StudioViewportPanels::StudioViewportPanels(
    StudioRenderViewSet& views,
    studio_session::StudioSession& session) noexcept
{
    Rebind(views, session);
}

void StudioViewportPanels::Rebind(
    StudioRenderViewSet& views,
    studio_session::StudioSession& session)
{
    views_ = &views;
    session_ = &session;
    views_->CreateDefaults();
    status_.clear();
}

void StudioViewportPanels::ClearBinding() noexcept
{
    views_ = nullptr;
    session_ = nullptr;
    status_.clear();
}

void StudioViewportPanels::Register(
    editor_ui::EditorUi& ui)
{
    if (views_ != nullptr)
    {
        views_->CreateDefaults();
    }

    ui.RegisterPanel({
        .id = kPrimaryViewportPanel,
        .title = "Viewport",
        .defaultOpen = true,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawView(context, "studio.primary");
            }
    });

    ui.RegisterPanel({
        .id = kSecondaryViewportPanel,
        .title = "Body Map / Debug View",
        .defaultOpen = true,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawView(context, "studio.map");
            }
    });
}

void StudioViewportPanels::RegisterSecondary(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kSecondaryViewportPanel,
        .title = "Body Map / Debug View",
        .defaultOpen = true,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawView(context, "studio.map");
            }
    });
}

void StudioViewportPanels::DrawView(
    editor_ui::PanelContext& context,
    const std::string_view id)
{
    if (views_ == nullptr || session_ == nullptr)
    {
        context.Text("Open or create a project to activate this viewport.");
        return;
    }

    auto* renderView = views_->Find(id);
    const auto* target = session_->Viewports().Find(id);

    if (renderView == nullptr || target == nullptr)
    {
        context.Text("Viewport is not registered.");
        return;
    }

    context.Text(
        std::format(
            "Mode: {}",
            ModeName(target->mode)));

    const std::string perspectiveButton =
        "Perspective##" + std::string(id);
    const std::string mapButton =
        "Body Map##" + std::string(id);
    const std::string debugButton =
        "Debug##" + std::string(id);

    if (context.Button(perspectiveButton))
    {
        session_->Viewports().SetMode(
            id,
            studio_session::ViewportMode::Perspective);
    }
    context.SameLine();
    if (context.Button(mapButton))
    {
        session_->Viewports().SetMode(
            id,
            studio_session::ViewportMode::BodyMap);
    }
    context.SameLine();
    if (context.Button(debugButton))
    {
        session_->Viewports().SetMode(
            id,
            studio_session::ViewportMode::Debug);
    }

    const std::string followButton =
        "Follow Active Body##" + std::string(id);
    if (context.Button(followButton))
    {
        session_->Viewports().FollowActiveBody(id);
        status_ = "Viewport now follows the shared active body.";
    }

    if (target->target.has_value())
    {
        context.Text(
            std::format(
                "Target: {}",
                target->target->name));
        context.Text(
            std::format(
                "Body {}",
                target->target->body.ToString()));
        context.Text(
            std::format(
                "Frame {}",
                target->target->frame.ToString()));
    }
    else
    {
        context.Text("Target: <none>");
    }

    if (session_->World().HasWorld())
    {
        context.Separator();
        context.Text("Pin body");

        const auto& universe =
            session_->World().Universe();
        const auto& bodies = universe.Bodies();

        for (const auto systemId : bodies.Systems())
        {
            const auto* system =
                bodies.FindSystem(systemId);

            if (system != nullptr)
            {
                context.Text(system->name);
            }

            for (const auto bodyId :
                 bodies.Bodies(systemId))
            {
                const auto* body =
                    bodies.FindBody(bodyId);
                const auto semantic =
                    universe.ObjectForBody(bodyId);

                if (body == nullptr ||
                    !semantic.has_value())
                {
                    continue;
                }

                const bool selected =
                    target->target.has_value() &&
                    target->target->semanticObject ==
                        *semantic;
                const std::string label =
                    body->name +
                    "##viewport-body:" +
                    std::string(id) + ":" +
                    semantic->ToString();

                if (context.Selectable(label, selected))
                {
                    try
                    {
                        session_->Viewports().PinToObject(
                            id,
                            *semantic);
                        status_ =
                            "Viewport pinned to " +
                            body->name + ".";
                    }
                    catch (const std::exception& exception)
                    {
                        status_ = exception.what();
                    }
                }
            }
        }

        if (const auto edge = SelectedBezierEdge(*session_);
            edge.has_value())
        {
            context.Separator();
            context.Text("Bezier handles (metres, edge evaluation frame)");

            math::Double3 startHandle =
                edge->startHandleMeters;
            math::Double3 endHandle =
                edge->endHandleMeters;

            const std::string startLabel =
                "Start Handle##" + std::string(id);
            const std::string endLabel =
                "End Handle##" + std::string(id);

            bool changed =
                context.InputDouble3(
                    startLabel,
                    startHandle);
            changed =
                context.InputDouble3(
                    endLabel,
                    endHandle) ||
                changed;

            if (changed)
            {
                try
                {
                    session_->PathNetwork().
                        Service().SetBezierHandles(
                            edge->id,
                            startHandle,
                            endHandle);
                    status_ =
                        "Bezier handles updated. Undo/redo uses the shared command transaction stack.";
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
        context.Text(status_);
    }

    context.Separator();
    const auto available = context.ContentAvailable();
    const u32 width =
        static_cast<u32>(
            std::max(available.width, 1.0F));
    const u32 height =
        static_cast<u32>(
            std::max(available.height, 1.0F));

    if (renderView->Width() != width ||
        renderView->Height() != height)
    {
        views_->Resize(id, width, height);
        renderView = views_->Find(id);
    }

    static_cast<void>(
        context.Image(
            renderView->Color(),
            {
                .width = static_cast<f32>(
                    renderView->Width()),
                .height = static_cast<f32>(
                    renderView->Height())
            }));
}
} // namespace orbit::studio_ui
