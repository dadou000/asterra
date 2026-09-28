#include <orbit/studio_ui/StudioViewportPanels.hpp>

#include <orbit/editor_model/CelestialAuthoringModel.hpp>
#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/studio_ui/CelestialAuthoringUi.hpp>
#include <orbit/studio_ui/SurfaceAuthoringUi.hpp>
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <exception>
#include <format>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#define Register RegisterBase
#define RegisterSecondary RegisterSecondaryBase
#define DrawView DrawViewBase
#include "StudioViewportPanelsBase.cpp"
#undef DrawView
#undef RegisterSecondary
#undef Register

namespace orbit::studio_ui
{
namespace
{
void ClosePanels(
    editor_ui::EditorUi& ui,
    const std::initializer_list<std::string_view> titles)
{
    for (const std::string_view title : titles)
    {
        static_cast<void>(
            ui.ClosePanelByTitle(title));
    }
}

void OpenPanels(
    editor_ui::EditorUi& ui,
    const std::initializer_list<std::string_view> titles)
{
    for (const std::string_view title : titles)
    {
        static_cast<void>(
            ui.FocusPanelByTitle(title));
    }
}

void CloseSpecialistPanels(editor_ui::EditorUi& ui)
{
    ClosePanels(
        ui,
        {
            "Inspector",
            "Body Map / Debug View",
            "System View",
            "Shading",
            "Shading Materials",
            "Celestial",
            "Surface Authoring",
            "Volumes",
            "Debug",
            "Display Diagnostics",
            "World Documents",
            "Project Settings"
        });
}

void OpenStandardInspectorWorkspace(
    editor_ui::EditorUi& ui,
    const std::initializer_list<std::string_view> centerPanels)
{
    CloseSpecialistPanels(ui);
    ClosePanels(
        ui,
        {
            "Properties",
            "Material Service",
            "Build",
            "Output"
        });

    OpenPanels(ui, {"Explorer"});
    OpenPanels(ui, centerPanels);
    OpenPanels(ui, {"Inspector"});
}

void RegisterWorkspaceActions(editor_ui::EditorUi& ui)
{
    ui.RegisterMenuAction({
        .menu = "Home",
        .label = "Workspace: Scene",
        .invoke = [&ui]
        {
            OpenStandardInspectorWorkspace(
                ui,
                {"Viewport"});
        }
    });

    ui.RegisterMenuAction({
        .menu = "Home",
        .label = "Workspace: Planet",
        .invoke = [&ui]
        {
            OpenStandardInspectorWorkspace(
                ui,
                {"Viewport"});
        }
    });

    ui.RegisterMenuAction({
        .menu = "Home",
        .label = "Workspace: Celestial",
        .invoke = [&ui]
        {
            CloseSpecialistPanels(ui);
            ClosePanels(
                ui,
                {
                    "Viewport",
                    "Properties",
                    "Material Service",
                    "Build",
                    "Output"
                });
            OpenPanels(
                ui,
                {"Explorer", "System View", "Inspector"});
        }
    });

    ui.RegisterMenuAction({
        .menu = "Home",
        .label = "Workspace: Simulation",
        .invoke = [&ui]
        {
            OpenStandardInspectorWorkspace(
                ui,
                {"Viewport"});
        }
    });

    ui.RegisterMenuAction({
        .menu = "Home",
        .label = "Workspace: Shading",
        .invoke = [&ui]
        {
            CloseSpecialistPanels(ui);
            ClosePanels(
                ui,
                {
                    "Viewport",
                    "Explorer",
                    "Properties",
                    "Material Service",
                    "Build",
                    "Output"
                });
            OpenPanels(
                ui,
                {"Shading", "Shading Materials"});
        }
    });
}
} // namespace

StudioViewportPanels::~StudioViewportPanels() = default;

void StudioViewportPanels::EnsureContextAuthoring()
{
    if (session_ == nullptr)
    {
        contextualSurface_.reset();
        contextualCelestial_.reset();
        contextualSession_ = nullptr;
        return;
    }

    if (contextualSession_ == session_ &&
        contextualSurface_ != nullptr &&
        contextualCelestial_ != nullptr)
    {
        return;
    }

    contextualSurface_ =
        std::make_unique<SurfaceAuthoringUi>(
            *session_);
    contextualCelestial_ =
        std::make_unique<CelestialAuthoringUi>(
            *session_);
    contextualSession_ =
        session_;
    contextualAdvancedProperties_ =
        false;
}

void StudioViewportPanels::RegisterContextInspector(
    editor_ui::EditorUi& ui)
{
    if (ui.HasPanel(kContextInspectorPanel))
    {
        return;
    }

    ui.RegisterPanel({
        .id = kContextInspectorPanel,
        .title = "Inspector",
        .defaultOpen = true,
        .defaultDock = editor_ui::DockRegion::Right,
        .dockOrder = -100,
        .minSize = {
            .width = 300.0F,
            .height = 300.0F
        },
        .defaultSize = {
            .width = 390.0F,
            .height = 820.0F
        },
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawContextInspector(context);
            }
    });
}

void StudioViewportPanels::DrawContextInspector(
    editor_ui::PanelContext& context)
{
    EnsureContextAuthoring();

    if (session_ == nullptr ||
        !session_->World().HasWorld())
    {
        context.Text(
            "Open a world to inspect authored objects.");
        return;
    }

    auto& world =
        session_->World();
    auto& inspector =
        world.Inspector();

    const auto selected =
        inspector.SelectedObjects();

    context.Heading("Inspector");

    if (selected.empty())
    {
        context.Text("No selection");
        context.MutedText(
            "Select an object in Explorer or the active workspace. Relevant authoring controls appear here automatically.");
        return;
    }

    if (selected.size() == 1U)
    {
        const auto& record =
            selected.front();

        context.Text(record.name);
        context.MutedText(
            record.type.ToString());
    }
    else
    {
        context.Text(
            std::format(
                "{} objects selected",
                selected.size()));
    }

    if (context.Section(
            "Properties##context-inspector-properties",
            true))
    {
        static_cast<void>(
            context.Checkbox(
                "Advanced##context-inspector-advanced",
                contextualAdvancedProperties_));

        if (!contextualAdvancedProperties_)
        {
            context.MutedText(
                "Advanced schema fields stay available here without becoming the default editing surface.");
        }

        context.Separator();

        for (auto property :
             inspector.CommonProperties())
        {
            if (property.schema.advanced &&
                !contextualAdvancedProperties_)
            {
                continue;
            }

            context.Text(
                std::format(
                    "{}{}{}",
                    property.schema.name,
                    property.schema.unit.empty()
                        ? ""
                        : " [",
                    property.schema.unit.empty()
                        ? ""
                        : property.schema.unit + "]"));

            if (property.mixed)
            {
                context.MutedText("<mixed>");
            }

            if (property.schema.readOnly)
            {
                context.MutedText("<read only>");
                context.Separator();
                continue;
            }

            const std::string label =
                "##context-property-" +
                property.schema.id.ToString();

            bool changed = false;

            std::visit(
                [&](auto& value)
                {
                    using Value =
                        std::decay_t<
                            decltype(value)>;

                    if constexpr (
                        std::is_same_v<Value, bool>)
                    {
                        changed =
                            context.Checkbox(
                                label,
                                value);
                    }
                    else if constexpr (
                        std::is_same_v<Value, i64>)
                    {
                        changed =
                            context.InputInteger(
                                label,
                                value);
                    }
                    else if constexpr (
                        std::is_same_v<Value, f64>)
                    {
                        changed =
                            context.InputDouble(
                                label,
                                value);
                    }
                    else if constexpr (
                        std::is_same_v<Value, std::string>)
                    {
                        changed =
                            context.InputText(
                                label,
                                value);
                    }
                    else if constexpr (
                        std::is_same_v<
                            Value,
                            math::Double3>)
                    {
                        changed =
                            context.InputDouble3(
                                label,
                                value);
                    }
                    else
                    {
                        const scene::ObjectId id{
                            .high = value.high,
                            .low = value.low
                        };

                        context.MutedText(
                            id.IsValid()
                                ? id.ToString()
                                : "<none>");
                    }
                },
                property.value);

            if (changed)
            {
                try
                {
                    inspector.SetForSelection(
                        property.schema.id,
                        property.value);
                }
                catch (const std::exception& exception)
                {
                    status_ =
                        exception.what();
                }
            }

            context.Separator();
        }
    }

    bool surfaceRelevant = false;
    bool celestialRelevant = false;

    if (selected.size() == 1U)
    {
        try
        {
            editor_model::SurfaceAuthoringModel
                surfaceModel(
                    world.Objects(),
                    world.Commands(),
                    world.Selection());

            surfaceRelevant =
                surfaceModel.SelectedRockyBody().
                    has_value();
        }
        catch (const std::exception&)
        {
            surfaceRelevant = false;
        }

        try
        {
            editor_model::CelestialAuthoringModel
                celestialModel(
                    world.Objects(),
                    world.Schemas(),
                    world.Commands(),
                    world.Selection());

            const auto primary =
                celestialModel.PrimarySelection();

            celestialRelevant =
                celestialModel.SelectedBody().
                    has_value() ||
                (primary.has_value() &&
                 (primary->type ==
                      world_model::kCelestialSystemType ||
                  primary->type ==
                      world_model::kCelestialReferenceNodeType ||
                  primary->type ==
                      world_model::kRingBandType ||
                  celestialModel.IsCapabilityType(
                      primary->type)));
        }
        catch (const std::exception&)
        {
            celestialRelevant = false;
        }
    }

    VolumeAuthoringUi* const volumeAuthoring =
        VolumeAuthoringUi::ContextInstance();
    const bool volumeRelevant =
        volumeAuthoring != nullptr &&
        volumeAuthoring->RelevantToSelection();

    if (surfaceRelevant &&
        contextualSurface_ != nullptr &&
        context.Section(
            "Surface##context-inspector-surface",
            true))
    {
        contextualSurface_->Draw(context);
    }

    if (celestialRelevant &&
        contextualCelestial_ != nullptr &&
        context.Section(
            "Celestial##context-inspector-celestial",
            !surfaceRelevant))
    {
        contextualCelestial_->Draw(context);
    }

    if (volumeRelevant &&
        context.Section(
            "Volume##context-inspector-volume",
            !surfaceRelevant &&
                !celestialRelevant))
    {
        volumeAuthoring->Draw(context);
    }

    if (!surfaceRelevant &&
        !celestialRelevant &&
        !volumeRelevant)
    {
        context.MutedText(
            "No specialized authoring section is needed for this selection. Common schema properties remain fully editable above.");
    }

    if (!status_.empty())
    {
        context.Separator();
        context.Text(status_);
    }
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
        .defaultDock = editor_ui::DockRegion::Center,
        .dockOrder = 0,
        .minSize = {.width = 320.0F, .height = 200.0F},
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawView(context, "studio.primary");
            }
    });

    // Keep the main scene as the only first-run centre workspace. The map /
    // debug view remains registered and docked with the viewport when opened
    // from View, but no longer competes for attention on a fresh layout.
    ui.RegisterPanel({
        .id = kSecondaryViewportPanel,
        .title = "Body Map / Debug View",
        .defaultOpen = false,
        .defaultDock = editor_ui::DockRegion::Center,
        .dockOrder = 10,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawView(context, "studio.map");
            }
    });

    RegisterContextInspector(ui);
    RegisterWorkspaceActions(ui);
}

void StudioViewportPanels::RegisterSecondary(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kSecondaryViewportPanel,
        .title = "Body Map / Debug View",
        .defaultOpen = false,
        .defaultDock = editor_ui::DockRegion::Center,
        .dockOrder = 10,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawView(context, "studio.map");
            }
    });

    RegisterContextInspector(ui);
    RegisterWorkspaceActions(ui);
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
