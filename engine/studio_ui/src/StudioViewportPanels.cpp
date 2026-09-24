#include <orbit/studio_ui/StudioViewportPanels.hpp>

#define Register RegisterBase
#define RegisterSecondary RegisterSecondaryBase
#define DrawView DrawViewBase
#include "StudioViewportPanelsBase.cpp"
#undef DrawView
#undef RegisterSecondary
#undef Register

namespace orbit::studio_ui
{
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
        .defaultDock = editor_ui::DockRegion::Center,
        .dockOrder = 0,
        .minSize = {.width = 320.0F, .height = 200.0F},
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
        .defaultDock = editor_ui::DockRegion::Center,
        .dockOrder = 10,
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
        .defaultDock = editor_ui::DockRegion::Center,
        .dockOrder = 10,
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
    DrawViewBase(context, id);

    if (views_ == nullptr ||
        session_ == nullptr ||
        !session_->World().HasWorld())
    {
        return;
    }

    auto* renderView =
        views_->Find(id);

    if (renderView == nullptr)
    {
        return;
    }

    const auto& selected =
        session_->World().Selection().Ordered();

    if (selected.size() != 1U)
    {
        return;
    }

    const scene::ObjectId lightId =
        selected.front();
    const auto record =
        session_->World().Objects().Find(lightId);

    if (!record.has_value() ||
        (record->type != world_model::kPointLightType &&
         record->type != world_model::kSpotLightType))
    {
        return;
    }

    const bool spot =
        record->type == world_model::kSpotLightType;

    context.Separator();
    context.Text("Selected Light Tools");
    context.MutedText(
        "Uses the authored light schema and command history; viewport range/cone gizmos update from these same properties.");

    const auto moveToView =
        [this, renderView, lightId, spot]
        {
            auto& commands =
                session_->World().Commands();
            const auto& camera =
                renderView->Camera();

            const math::Double3 position{
                camera.localPositionMeters.x +
                    static_cast<f64>(camera.forward.x) * 5.0,
                camera.localPositionMeters.y +
                    static_cast<f64>(camera.forward.y) * 5.0,
                camera.localPositionMeters.z +
                    static_cast<f64>(camera.forward.z) * 5.0
            };

            commands.BeginTransaction(
                spot
                    ? "Move Spot Light To View"
                    : "Move Point Light To View");

            try
            {
                commands.SetProperty(
                    lightId,
                    world_model::kLightPositionMeters,
                    position);
                commands.CommitTransaction();
            }
            catch (...)
            {
                if (commands.HasActiveTransaction())
                {
                    commands.RollbackTransaction();
                }
                throw;
            }
        };

    const auto aimAlongView =
        [this, renderView, lightId]
        {
            auto& commands =
                session_->World().Commands();
            const auto& forward =
                renderView->Camera().forward;

            commands.BeginTransaction(
                "Aim Spot Light Along View");

            try
            {
                commands.SetProperty(
                    lightId,
                    world_model::kLightDirection,
                    math::Double3{
                        static_cast<f64>(forward.x),
                        static_cast<f64>(forward.y),
                        static_cast<f64>(forward.z)
                    });
                commands.CommitTransaction();
            }
            catch (...)
            {
                if (commands.HasActiveTransaction())
                {
                    commands.RollbackTransaction();
                }
                throw;
            }
        };

    const std::string moveLabel =
        "Move Light To View##m41-light-move:" +
        std::string(id);

    if (context.Button(moveLabel))
    {
        try
        {
            moveToView();
            status_ =
                "Selected light moved 5 m in front of the active view.";
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
        }
    }

    if (spot)
    {
        context.SameLine();

        const std::string aimLabel =
            "Aim Spot Along View##m41-light-aim:" +
            std::string(id);

        if (context.Button(aimLabel))
        {
            try
            {
                aimAlongView();
                status_ =
                    "Selected spot light aimed along the active view.";
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
            }
        }
    }
}
} // namespace orbit::studio_ui
