#include <orbit/studio_ui/StudioExpansionShell.hpp>

#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/editor_ui/FocusState.hpp>
#include <orbit/editor_ui/PanelExtensions.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/studio_ui/CommandPaletteModel.hpp>
#include <orbit/studio_ui/SelectionBreadcrumbs.hpp>
#include <orbit/studio_ui/StudioViewportPanels.hpp>
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>
#include <orbit/terrain_debug/TerrainDebugField.hpp>
#include <orbit/terrain_debug/TerrainDebugSeam.hpp>
#include <orbit/world_model/VisibilityProxyBinding.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orbit::studio_ui
{
namespace
{
InspectorProviderRegistry g_inspectorProviders;
StudioUiContributionRegistry g_uiContributions;

constexpr std::array<std::string_view, 9> kTerrainTools{
    "Select",
    "Raise",
    "Lower",
    "Protect",
    "Drainage",
    "Canyon",
    "Ridge",
    "Geology",
    "Biome Paint"
};

constexpr std::array<std::string_view, 6> kBiomeOperations{
    "Add",
    "Subtract",
    "Replace",
    "Multiply",
    "Min",
    "Max"
};

constexpr std::array<std::string_view, 3> kViewportControlModes{
    "Auto",
    "Primary",
    "Body Map"
};

constexpr std::array<std::string_view, 4> kSurfaceViews{
    "Lit",
    "Albedo",
    "Normal",
    "Emission"
};

[[nodiscard]] bool IsQuickCreateLabel(
    const std::string_view label) noexcept
{
    return label.starts_with("Add ") ||
        label.starts_with("Create ") ||
        label.starts_with("New ");
}

[[nodiscard]] bool IsTerrainSplineTool(
    const StudioTerrainAuthoringTool tool) noexcept
{
    return tool == StudioTerrainAuthoringTool::Canyon ||
        tool == StudioTerrainAuthoringTool::Ridge;
}

[[nodiscard]] const char* ViewportModeName(
    const studio_session::ViewportMode mode) noexcept
{
    switch (mode)
    {
    case studio_session::ViewportMode::Perspective: return "Perspective";
    case studio_session::ViewportMode::BodyMap: return "Body Map";
    case studio_session::ViewportMode::Debug: return "Debug";
    case studio_session::ViewportMode::System: return "System";
    }
    return "Perspective";
}

[[nodiscard]] const char* EdgeName(
    const world::TileEdge edge) noexcept
{
    switch (edge)
    {
    case world::TileEdge::North: return "N";
    case world::TileEdge::East: return "E";
    case world::TileEdge::South: return "S";
    case world::TileEdge::West: return "W";
    }
    return "?";
}

[[nodiscard]] const char* CubeFaceName(
    const world::CubeFace face) noexcept
{
    switch (face)
    {
    case world::CubeFace::PositiveX: return "+X";
    case world::CubeFace::NegativeX: return "-X";
    case world::CubeFace::PositiveY: return "+Y";
    case world::CubeFace::NegativeY: return "-Y";
    case world::CubeFace::PositiveZ: return "+Z";
    case world::CubeFace::NegativeZ: return "-Z";
    }
    return "?";
}
} // namespace

bool StudioViewportPanels::CanCreateAtViewport(
    const std::string_view id) const noexcept
{
    if (views_ == nullptr ||
        session_ == nullptr ||
        !session_->World().HasWorld() ||
        views_->Find(id) == nullptr)
    {
        return false;
    }

    const auto* target =
        session_->Viewports().Find(id);
    return target != nullptr &&
        target->target.has_value();
}

void StudioViewportPanels::CreateLocalLightAtViewport(
    const std::string_view id,
    const bool spot)
{
    if (!CanCreateAtViewport(id))
    {
        throw std::runtime_error(
            "A targeted viewport is required to add a local light.");
    }

    auto* renderView = views_->Find(id);
    const auto* target = session_->Viewports().Find(id);
    auto& world = session_->World();

    const auto bodyObject =
        world.Universe().ObjectForBody(
            target->target->body);

    if (!bodyObject.has_value())
    {
        throw std::runtime_error(
            "Target body has no semantic object for local-light parenting.");
    }

    auto& commands = world.Commands();
    commands.BeginTransaction(
        spot ? "Add Spot Light" : "Add Point Light");

    scene::ObjectId created{};
    try
    {
        created = commands.CreateObject(
            spot
                ? world_model::kSpotLightType
                : world_model::kPointLightType,
            spot ? "Spot Light" : "Point Light",
            *bodyObject);

        const auto& camera = renderView->Camera();
        const math::Double3 position{
            camera.localPositionMeters.x +
                static_cast<f64>(camera.forward.x) * 5.0,
            camera.localPositionMeters.y +
                static_cast<f64>(camera.forward.y) * 5.0,
            camera.localPositionMeters.z +
                static_cast<f64>(camera.forward.z) * 5.0
        };

        commands.SetProperty(
            created,
            world_model::kLightPositionMeters,
            position);

        if (spot)
        {
            commands.SetProperty(
                created,
                world_model::kLightDirection,
                math::Double3{
                    static_cast<f64>(camera.forward.x),
                    static_cast<f64>(camera.forward.y),
                    static_cast<f64>(camera.forward.z)
                });
        }

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

    const std::array selected{created};
    world.Selection().Set(
        std::span<const scene::ObjectId>(selected));
    status_ = spot
        ? "Spot light created and selected."
        : "Point light created and selected.";
}

void StudioViewportPanels::CreateVisibilityProxyAtViewport(
    const std::string_view id,
    const bool box)
{
    if (!CanCreateAtViewport(id))
    {
        throw std::runtime_error(
            "A targeted viewport is required to add a visibility proxy.");
    }

    auto* renderView = views_->Find(id);
    const auto* target = session_->Viewports().Find(id);
    auto& world = session_->World();

    const auto bodyObject =
        world.Universe().ObjectForBody(
            target->target->body);

    if (!bodyObject.has_value())
    {
        throw std::runtime_error(
            "Target body has no semantic object for visibility-proxy parenting.");
    }

    auto& commands = world.Commands();
    commands.BeginTransaction(
        box
            ? "Add Box Visibility Proxy"
            : "Add Sphere Visibility Proxy");

    scene::ObjectId created{};
    try
    {
        created = commands.CreateObject(
            world_model::kVisibilityProxyType,
            box
                ? "Box Visibility Proxy"
                : "Sphere Visibility Proxy",
            *bodyObject);

        const auto& camera = renderView->Camera();
        const math::Double3 position{
            camera.localPositionMeters.x +
                static_cast<f64>(camera.forward.x) * 5.0,
            camera.localPositionMeters.y +
                static_cast<f64>(camera.forward.y) * 5.0,
            camera.localPositionMeters.z +
                static_cast<f64>(camera.forward.z) * 5.0
        };

        commands.SetProperty(
            created,
            world_model::kVisibilityProxyPositionMeters,
            position);

        if (box)
        {
            commands.SetProperty(
                created,
                world_model::kVisibilityProxyShape,
                i64{1});
            commands.SetProperty(
                created,
                world_model::kVisibilityProxyHalfExtentsMeters,
                math::Double3{1.0, 1.0, 1.0});
        }
        else
        {
            commands.SetProperty(
                created,
                world_model::kVisibilityProxyShape,
                i64{0});
            commands.SetProperty(
                created,
                world_model::kVisibilityProxyRadiusMeters,
                1.0);
        }

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

    const std::array selected{created};
    world.Selection().Set(
        std::span<const scene::ObjectId>(selected));
    status_ = box
        ? "Box visibility proxy created and selected."
        : "Sphere visibility proxy created and selected.";
}

InspectorProviderRegistry& GlobalInspectorProviders() noexcept
{
    return g_inspectorProviders;
}

StudioUiContributionRegistry& GlobalStudioUiContributions() noexcept
{
    return g_uiContributions;
}

StudioExpansionShell::StudioExpansionShell(
    StudioViewportPanels& owner) noexcept
    : owner_(&owner)
{
    try
    {
        // Two top rows only: workspace/navigation and contextual authoring.
        // The bottom activity strip is owned by StudioViewportPanels because
        // it directly controls that object's build/log/diagnostic panels.
        editor_ui::UpsertShellBand({
            .id = "orbit.navigation",
            .order = 0,
            .height = 40.0F,
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawNavigationBand(context);
                }
        });

        editor_ui::UpsertShellBand({
            .id = "orbit.viewport-authoring",
            .order = 10,
            .height = 42.0F,
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawViewportBand(context);
                }
        });

        // Plugin/context providers extend the same canonical Properties panel
        // as built-in authoring tools. The legacy Inspector remains available
        // as an expert compatibility surface, but it is no longer a second
        // default property-editing destination.
        editor_ui::UpsertPanelExtension({
            .id = "orbit.inspector.providers",
            .targetTitle = "Properties",
            .order = 1'100,
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawInspectorExtension(context);
                }
        });

        GlobalInspectorProviders().Upsert({
            .id = "orbit.viewport.target",
            .owner = "orbit",
            .title = "Viewport Target",
            .order = 20,
            .defaultOpen = false,
            .relevant =
                [this]
                {
                    return ViewportControlsRelevant();
                },
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawViewportTargetProperties(context);
                }
        });

        GlobalInspectorProviders().Upsert({
            .id = "orbit.path.bezier",
            .owner = "orbit",
            .title = "Bezier Path",
            .order = 60,
            .defaultOpen = true,
            .relevant =
                [this]
                {
                    return BezierContextRelevant();
                },
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawBezierProperties(context);
                }
        });

        // Terrain used to occupy most of the permanent context row with nine
        // buttons. Keep one selector in the shell and move only the active
        // tool's parameters into canonical Properties.
        GlobalInspectorProviders().Upsert({
            .id = "orbit.terrain.active-tool",
            .owner = "orbit",
            .title = "Terrain Tool",
            .order = 80,
            .defaultOpen = true,
            .relevant =
                [this]
                {
                    return TerrainContextRelevant();
                },
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawTerrainToolProperties(context);
                }
        });

        GlobalInspectorProviders().Upsert({
            .id = "orbit.viewport.diagnostics",
            .owner = "orbit",
            .title = "Viewport Diagnostics",
            .order = 120,
            .defaultOpen = false,
            .relevant =
                [this]
                {
                    return ViewportControlsRelevant();
                },
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawViewportDiagnosticsProperties(context);
                }
        });

        attached_ = true;
    }
    catch (...)
    {
        // Registration is transactional from the shell owner's point of view:
        // never leave callbacks retaining this object after a partial setup.
        static_cast<void>(
            GlobalInspectorProviders().Remove(
                "orbit.viewport.diagnostics"));
        static_cast<void>(
            GlobalInspectorProviders().Remove(
                "orbit.terrain.active-tool"));
        static_cast<void>(
            GlobalInspectorProviders().Remove(
                "orbit.path.bezier"));
        static_cast<void>(
            GlobalInspectorProviders().Remove(
                "orbit.viewport.target"));
        static_cast<void>(
            editor_ui::RemovePanelExtension(
                "orbit.inspector.providers"));
        static_cast<void>(
            editor_ui::RemoveShellBand("orbit.navigation"));
        static_cast<void>(
            editor_ui::RemoveShellBand("orbit.viewport-authoring"));

        // Some headless/model tests construct StudioViewportPanels without an
        // active EditorUi/ImGui context. The model remains usable there and
        // the live app attaches these surfaces through normal construction.
        attached_ = false;
    }
}

StudioExpansionShell::~StudioExpansionShell()
{
    SavePersistentStateIfChanged();

    if (!attached_)
    {
        return;
    }

    static_cast<void>(
        GlobalInspectorProviders().Remove(
            "orbit.viewport.diagnostics"));
    static_cast<void>(
        GlobalInspectorProviders().Remove(
            "orbit.terrain.active-tool"));
    static_cast<void>(
        GlobalInspectorProviders().Remove(
            "orbit.path.bezier"));
    static_cast<void>(
        GlobalInspectorProviders().Remove(
            "orbit.viewport.target"));
    static_cast<void>(
        editor_ui::RemovePanelExtension(
            "orbit.inspector.providers"));
    static_cast<void>(
        editor_ui::RemoveShellBand("orbit.navigation"));
    static_cast<void>(
        editor_ui::RemoveShellBand("orbit.viewport-authoring"));
}

void StudioExpansionShell::SavePersistentStateIfChanged() noexcept
{
    if (!persistentStateLoaded_ ||
        persistentStatePath_.empty())
    {
        return;
    }

    persistentState_.viewport = viewportState_;
    if (owner_ != nullptr)
    {
        persistentState_.inspectorAdvanced =
            owner_->contextualAdvancedProperties_;
    }

    const std::string serialized =
        SerializeStudioPersistentState(
            persistentState_);

    if (serialized == persistentSnapshot_)
    {
        return;
    }

    try
    {
        SaveStudioPersistentState(
            persistentStatePath_,
            persistentState_);
        persistentSnapshot_ = serialized;
    }
    catch (const std::exception& exception)
    {
        if (owner_ != nullptr)
        {
            owner_->status_ =
                std::string{"Studio state save failed: "} +
                exception.what();
        }
    }
}

void StudioExpansionShell::SyncPersistentState() noexcept
{
    studio_session::StudioSession* const session =
        owner_ != nullptr
            ? owner_->session_
            : nullptr;

    if (session == persistentSession_ &&
        persistentStateLoaded_)
    {
        SavePersistentStateIfChanged();
        return;
    }

    // Rebinding projects is a normal Studio operation. Persist the old
    // project before swapping the presentation binding.
    SavePersistentStateIfChanged();

    persistentSession_ = session;
    persistentStatePath_.clear();
    persistentState_ = {};
    persistentSnapshot_.clear();
    persistentStateLoaded_ = false;

    if (session == nullptr)
    {
        return;
    }

    try
    {
        persistentStatePath_ =
            session->World().Project().RootDirectory() /
            ".orbit" /
            "StudioState.ini";

        persistentState_ =
            LoadStudioPersistentState(
                persistentStatePath_);

        viewportState_ = persistentState_.viewport;
        if (owner_ != nullptr)
        {
            owner_->contextualAdvancedProperties_ =
                persistentState_.inspectorAdvanced;
        }

        persistentSnapshot_ =
            SerializeStudioPersistentState(
                persistentState_);
        persistentStateLoaded_ = true;
    }
    catch (const std::exception& exception)
    {
        if (owner_ != nullptr)
        {
            owner_->status_ =
                std::string{"Studio state load failed: "} +
                exception.what();
        }
    }
}

void StudioExpansionShell::DrawContributions(
    editor_ui::PanelContext& context,
    const StudioContributionSurface surface)
{
    if (owner_ == nullptr ||
        owner_->session_ == nullptr ||
        !owner_->session_->World().HasWorld())
    {
        return;
    }

    auto& commands =
        owner_->session_->World().CommandRegistry();

    std::vector<editor_ui::ActionPresentation> actions;
    for (const auto& contribution :
         GlobalStudioUiContributions().Catalog(surface))
    {
        if (contribution.kind != StudioContributionKind::Command ||
            !contribution.command.IsValid())
        {
            continue;
        }

        const auto enablement =
            commands.Enablement(contribution.command);
        const auto command = contribution.command;

        actions.push_back({
            .label = contribution.label,
            .enabled = enablement.enabled,
            .disabledReason = enablement.reason,
            .invoke =
                [this, command]
                {
                    if (owner_ == nullptr ||
                        owner_->session_ == nullptr ||
                        !owner_->session_->World().HasWorld())
                    {
                        return;
                    }

                    try
                    {
                        owner_->session_->World().CommandRegistry().Invoke(
                            command);
                        owner_->status_.clear();
                    }
                    catch (const std::exception& exception)
                    {
                        owner_->status_ = exception.what();
                    }
                }
        });
    }

    if (!actions.empty())
    {
        context.SameLine();
        context.Toolbar(actions);
    }
}

void StudioExpansionShell::DrawInspectorExtension(
    editor_ui::PanelContext& context)
{
    SyncPersistentState();

    const auto providers =
        GlobalInspectorProviders().Relevant();

    if (providers.empty())
    {
        return;
    }

    context.Separator();
    context.MutedText("Contextual");
    static_cast<void>(
        GlobalInspectorProviders().DrawRelevant(
            context));
}

std::string_view StudioExpansionShell::SelectedViewportId() const noexcept
{
    if (viewportControlMode_ == 1)
    {
        return "studio.primary";
    }
    if (viewportControlMode_ == 2)
    {
        return "studio.map";
    }

    const std::string_view focused =
        editor_ui::FocusedWindowTitle();
    if (focused == "Viewport")
    {
        lastFocusedViewportIndex_ = 0;
    }
    else if (focused == "Body Map / Debug View")
    {
        lastFocusedViewportIndex_ = 1;
    }

    return lastFocusedViewportIndex_ == 1
        ? std::string_view{"studio.map"}
        : std::string_view{"studio.primary"};
}

bool StudioExpansionShell::ViewportControlsRelevant() const noexcept
{
    if (owner_ == nullptr ||
        owner_->views_ == nullptr ||
        owner_->session_ == nullptr ||
        !owner_->session_->World().HasWorld())
    {
        return false;
    }

    const std::string_view id = SelectedViewportId();
    return owner_->views_->Find(id) != nullptr &&
        owner_->session_->Viewports().Find(id) != nullptr;
}

bool StudioExpansionShell::BezierContextRelevant() const noexcept
{
    if (owner_ == nullptr ||
        owner_->session_ == nullptr ||
        !owner_->session_->World().HasWorld())
    {
        return false;
    }

    auto& paths = owner_->session_->PathNetwork().Service();
    for (const auto object :
         owner_->session_->World().Selection().Ordered())
    {
        const auto edge = paths.FindEdge(object);
        if (edge.has_value() &&
            edge->mode == paths::EdgeMode::Bezier)
        {
            return true;
        }
    }
    return false;
}

void StudioExpansionShell::DrawBuiltInQuickCreate(
    editor_ui::PanelContext& context)
{
    if (owner_ == nullptr)
    {
        return;
    }

    const std::string viewportId{SelectedViewportId()};
    const bool enabled =
        owner_->CanCreateAtViewport(viewportId);
    const std::string disabledReason =
        enabled
            ? std::string{}
            : std::string{
                "The controlled viewport must target a world body."};

    std::vector<editor_ui::ActionPresentation> actions;
    actions.reserve(4U);

    actions.push_back({
        .label = "Point Light##quick-add-point-light",
        .enabled = enabled,
        .disabledReason = disabledReason,
        .invoke =
            [this, viewportId]
            {
                try
                {
                    owner_->CreateLocalLightAtViewport(
                        viewportId,
                        false);
                    quickCreateOpen_ = false;
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }
    });
    actions.push_back({
        .label = "Spot Light##quick-add-spot-light",
        .enabled = enabled,
        .disabledReason = disabledReason,
        .invoke =
            [this, viewportId]
            {
                try
                {
                    owner_->CreateLocalLightAtViewport(
                        viewportId,
                        true);
                    quickCreateOpen_ = false;
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }
    });
    actions.push_back({
        .label = "Sphere Proxy##quick-add-sphere-proxy",
        .enabled = enabled,
        .disabledReason = disabledReason,
        .invoke =
            [this, viewportId]
            {
                try
                {
                    owner_->CreateVisibilityProxyAtViewport(
                        viewportId,
                        false);
                    quickCreateOpen_ = false;
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }
    });
    actions.push_back({
        .label = "Box Proxy##quick-add-box-proxy",
        .enabled = enabled,
        .disabledReason = disabledReason,
        .invoke =
            [this, viewportId]
            {
                try
                {
                    owner_->CreateVisibilityProxyAtViewport(
                        viewportId,
                        true);
                    quickCreateOpen_ = false;
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }
    });

    context.SameLine();
    context.Toolbar(actions);
}

void StudioExpansionShell::DrawViewportTargetProperties(
    editor_ui::PanelContext& context)
{
    if (!ViewportControlsRelevant())
    {
        return;
    }

    viewportControlMode_ = std::clamp(viewportControlMode_, 0, 2);
    static_cast<void>(
        context.Combo(
            "Control##viewport-control-mode",
            kViewportControlModes,
            viewportControlMode_));

    static constexpr std::array<std::string_view, 4>
        kLayouts{"Single", "Vertical", "Horizontal", "Quad"};
    i32 layout = static_cast<i32>(viewportState_.layout);
    if (context.Combo(
            "Layout##viewport-layout-properties",
            kLayouts,
            layout))
    {
        viewportState_.SetLayout(
            static_cast<ViewportLayout>(layout));
    }

    static constexpr std::array<std::string_view, 2>
        kTransformSpaces{"World", "Local"};
    i32 transformSpace = static_cast<i32>(viewportState_.gizmo.space);
    if (context.Combo(
            "Transform Space##viewport-transform-space-properties",
            kTransformSpaces,
            transformSpace))
    {
        transformSpace = std::clamp(transformSpace, 0, 1);
        viewportState_.gizmo.space =
            static_cast<GizmoSpace>(transformSpace);
    }

    const std::string_view id = SelectedViewportId();
    auto& session = *owner_->session_;
    const auto* target = session.Viewports().Find(id);

    context.Text(
        id == "studio.map"
            ? "Controlled viewport: Body Map / Debug View"
            : "Controlled viewport: Primary");
    context.MutedText(
        viewportControlMode_ == 0
            ? "Auto follows the last focused production viewport."
            : "Viewport control is pinned until Control returns to Auto.");
    context.MutedText(
        std::format(
            "Mode: {}",
            ViewportModeName(target->mode)));

    if (context.Button("Follow Active Body##viewport-follow-active"))
    {
        try
        {
            session.Viewports().FollowActiveBody(id);
            owner_->status_ =
                "Controlled viewport now follows the shared active body.";
        }
        catch (const std::exception& exception)
        {
            owner_->status_ = exception.what();
        }
    }

    if (target->target.has_value())
    {
        context.KeyValue("Target", target->target->name);
        context.KeyValue("Body", target->target->body.ToString());
        context.KeyValue("Frame", target->target->frame.ToString());

        const auto bodyObject =
            session.World().Universe().ObjectForBody(
                target->target->body);
        if (bodyObject.has_value())
        {
            const auto proxies =
                world_model::ResolveVisibilityProxies(
                    session.World().Objects(),
                    *bodyObject);
            const auto dynamicCount =
                std::count_if(
                    proxies.begin(),
                    proxies.end(),
                    [](const auto& proxy)
                    {
                        return proxy.dynamic;
                    });
            context.KeyValue(
                "Visibility proxies",
                std::format(
                    "{} authored · {} dynamic",
                    proxies.size(),
                    dynamicCount));
        }
    }
    else
    {
        context.MutedText("Target: none");
    }

    context.Separator();
    context.MutedText("Pin body");

    const auto& universe = session.World().Universe();
    const auto& bodies = universe.Bodies();
    for (const auto systemId : bodies.Systems())
    {
        const auto* system = bodies.FindSystem(systemId);
        if (system != nullptr)
        {
            context.Text(system->name);
        }

        for (const auto bodyId : bodies.Bodies(systemId))
        {
            const auto* body = bodies.FindBody(bodyId);
            const auto semantic = universe.ObjectForBody(bodyId);
            if (body == nullptr || !semantic.has_value())
            {
                continue;
            }

            const bool selected =
                target->target.has_value() &&
                target->target->semanticObject == *semantic;
            std::string label = body->name;
            label += "##viewport-target-body:";
            label += semantic->ToString();

            if (context.Selectable(label, selected))
            {
                try
                {
                    session.Viewports().PinToObject(
                        id,
                        *semantic);
                    owner_->status_ =
                        "Viewport pinned to " + body->name + ".";
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }
        }
    }
}

void StudioExpansionShell::DrawBezierProperties(
    editor_ui::PanelContext& context)
{
    if (!BezierContextRelevant())
    {
        return;
    }

    auto& paths = owner_->session_->PathNetwork().Service();
    for (const auto object :
         owner_->session_->World().Selection().Ordered())
    {
        const auto edge = paths.FindEdge(object);
        if (!edge.has_value() ||
            edge->mode != paths::EdgeMode::Bezier)
        {
            continue;
        }

        math::Double3 startHandle = edge->startHandleMeters;
        math::Double3 endHandle = edge->endHandleMeters;

        bool changed = context.InputDouble3(
            "Start Handle (m)##bezier-start-handle",
            startHandle);
        changed = context.InputDouble3(
            "End Handle (m)##bezier-end-handle",
            endHandle) || changed;

        if (changed)
        {
            try
            {
                paths.SetBezierHandles(
                    edge->id,
                    startHandle,
                    endHandle);
                owner_->status_ =
                    "Bezier handles updated. Undo/redo uses the shared command transaction stack.";
            }
            catch (const std::exception& exception)
            {
                owner_->status_ = exception.what();
            }
        }
        return;
    }
}

void StudioExpansionShell::DrawViewportDiagnosticsProperties(
    editor_ui::PanelContext& context)
{
    if (!ViewportControlsRelevant())
    {
        return;
    }

    const std::string_view id = SelectedViewportId();
    const auto* target = owner_->session_->Viewports().Find(id);

    context.Text(
        id == "studio.map"
            ? "Diagnostics: Body Map / Debug View"
            : "Diagnostics: Primary");

    if (target->mode == studio_session::ViewportMode::Perspective)
    {
        auto diagnostics =
            owner_->views_->TerrainDiagnosticOverlays(id);
        bool changed = false;

        const auto toggle =
            [&context, &changed](
                const char* label,
                bool& value)
            {
                changed =
                    context.Checkbox(label, value) || changed;
            };

        toggle(
            "Dirty page bounds##diag-dirty",
            diagnostics.dirtyPageBounds);
        toggle(
            "Build / upload states##diag-build",
            diagnostics.buildStates);
        toggle(
            "Physical LOD##diag-physical-lod",
            diagnostics.physicalLod);
        toggle(
            "Clipmap rings##diag-clipmap-rings",
            diagnostics.clipmapRings);
        toggle(
            "Cache status##diag-cache",
            diagnostics.cacheStatus);
        toggle(
            "Authored constraints##diag-constraints",
            diagnostics.authoredConstraints);
        toggle(
            "Biome weights##diag-biome",
            diagnostics.biomeWeights);
        toggle(
            "Process masks##diag-process",
            diagnostics.processMasks);
        toggle(
            "Drainage vectors##diag-drainage",
            diagnostics.drainageVectors);

        if (changed)
        {
            owner_->views_->SetTerrainDiagnosticOverlays(
                id,
                diagnostics);
        }

        if (diagnostics.cacheStatus)
        {
            const auto runtime =
                owner_->session_->TerrainRuntime().Capture(id);
            if (runtime.has_value())
            {
                const auto* services =
                    owner_->session_->World().Surfaces().ServicesForBody(
                        runtime->body);
                if (services != nullptr)
                {
                    const auto stats = services->Cache().Stats();
                    context.Text(
                        std::format(
                            "M26 cache: {} pages · {} bytes · hits {} · misses {} · evictions {}",
                            stats.residentPages,
                            stats.residentBytes,
                            stats.hits,
                            stats.misses,
                            stats.evictions));
                }

                const auto rebuild =
                    owner_->session_->TerrainPhysicalPages().BodyStatus(
                        runtime->planet.id);
                if (rebuild.has_value())
                {
                    context.Text(
                        std::format(
                            "M06 pages: {} dirty · {} queued · {} building · {} uploading · {} ready",
                            rebuild->dirtyPages,
                            rebuild->queuedPages,
                            rebuild->buildingPages,
                            rebuild->uploadingPages,
                            rebuild->readyPages));
                }
            }
        }
        return;
    }

    if (target->mode != studio_session::ViewportMode::Debug)
    {
        context.MutedText(
            "Terrain diagnostics are available in Perspective or Debug mode.");
        return;
    }

    const auto catalog = terrain_debug::FieldCatalog();
    std::vector<std::string_view> fieldNames;
    fieldNames.reserve(catalog.size());

    i32 selectedFieldIndex = 0;
    const auto selectedField = owner_->views_->DebugField(id);
    for (std::size_t index = 0U; index < catalog.size(); ++index)
    {
        fieldNames.push_back(catalog[index].name);
        if (catalog[index].field == selectedField)
        {
            selectedFieldIndex = static_cast<i32>(index);
        }
    }

    if (!fieldNames.empty() &&
        context.Combo(
            "Field##viewport-debug-field",
            fieldNames,
            selectedFieldIndex))
    {
        selectedFieldIndex = std::clamp<i32>(
            selectedFieldIndex,
            0,
            static_cast<i32>(catalog.size()) - 1);
        owner_->views_->SetDebugField(
            id,
            catalog[static_cast<std::size_t>(selectedFieldIndex)].field);
    }

    i64 pageLevel = static_cast<i64>(
        owner_->views_->DebugPhysicalPageLevel(id));
    if (context.InputInteger(
            "Physical page tile level##viewport-debug-page-level",
            pageLevel))
    {
        pageLevel = std::clamp<i64>(pageLevel, 0, 30);
        owner_->views_->SetDebugPhysicalPageLevel(
            id,
            static_cast<u8>(pageLevel));
        owner_->status_ =
            "Physical page level changed; the center page will be reselected.";
    }

    const auto selectedPage = owner_->views_->DebugPhysicalPage(id);
    if (selectedPage.has_value())
    {
        const auto& tile = selectedPage->address.tile;
        context.Text(
            std::format(
                "Physical page: {} L{} ({}, {})",
                CubeFaceName(tile.face),
                tile.level,
                tile.x,
                tile.y));
    }
    else
    {
        context.MutedText(
            "Physical page: none · click terrain in the debug viewport.");
    }

    const auto livePage = owner_->views_->LiveDebugPage(id);
    if (livePage == nullptr)
    {
        context.MutedText(
            "Live products: no physical-page snapshot published yet.");
        return;
    }

    const auto field = owner_->views_->DebugField(id);
    context.Text(
        std::format(
            "Live products: ready · physical LOD {} · {}x{}",
            livePage->Stamp().physicalLod,
            livePage->Width(),
            livePage->Height()));
    context.Text(
        livePage->Has(field)
            ? "Selected field: available"
            : "Selected field: not published by the live page producer");

    if (!livePage->Has(field))
    {
        return;
    }

    const auto seams =
        terrain_debug::InspectTerrainDebugSeams(
            *livePage,
            field,
            owner_->session_->TerrainDebugPages());

    context.MutedText("Physical page seams");
    for (const auto& seam : seams)
    {
        if (seam.state ==
            terrain_debug::TerrainDebugSeamState::ValueMismatch)
        {
            context.Text(
                std::format(
                    "{}: {} ({}/{} samples, max diff {:.6g})",
                    EdgeName(seam.edge),
                    terrain_debug::TerrainDebugSeamStateName(seam.state),
                    seam.comparison.mismatchedSamples,
                    seam.comparison.samplesCompared,
                    seam.comparison.maximumDifference));
        }
        else
        {
            context.Text(
                std::format(
                    "{}: {}",
                    EdgeName(seam.edge),
                    terrain_debug::TerrainDebugSeamStateName(seam.state)));
        }
    }

    const auto& descriptor = terrain_debug::Descriptor(field);
    context.MutedText("Upstream provenance");
    for (const auto stage : descriptor.upstream)
    {
        context.Text(
            std::format(
                "- {}",
                terrain_debug::StageName(stage)));
    }
}

bool StudioExpansionShell::TerrainContextRelevant() const noexcept
{
    if (owner_ == nullptr ||
        owner_->session_ == nullptr ||
        !owner_->session_->World().HasWorld())
    {
        return false;
    }

    auto& world = owner_->session_->World();
    const auto& selection = world.Selection().Ordered();

    // Preserve the existing context-row precedence. Lights, path-pair actions
    // and volume authoring remain more specific than terrain ancestry.
    if (selection.size() == 1U)
    {
        const auto record =
            world.Objects().Find(selection.front());
        if (record.has_value() &&
            (record->type == world_model::kPointLightType ||
             record->type == world_model::kSpotLightType))
        {
            return false;
        }
    }

    if (world.CommandRegistry().Enablement(
            editor_model::authoring_commands::
                kConnectPathDirect).enabled)
    {
        return false;
    }

    if (auto* volume = VolumeAuthoringUi::ContextInstance();
        volume != nullptr &&
        volume->RelevantToSelection())
    {
        return false;
    }

    try
    {
        editor_model::SurfaceAuthoringModel model(
            world.Objects(),
            world.Commands(),
            world.Selection());
        return model.SelectedRockyBody().has_value();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

void StudioExpansionShell::DrawTerrainContext(
    editor_ui::PanelContext& context)
{
    if (owner_ == nullptr)
    {
        return;
    }

    context.Text("Terrain");
    context.SameLine();

    i32 tool =
        static_cast<i32>(owner_->terrainTool_);

    if (context.Combo(
            "##quick-terrain-tool",
            kTerrainTools,
            tool))
    {
        const auto next =
            static_cast<StudioTerrainAuthoringTool>(tool);

        if (owner_->terrainTool_ != next &&
            (IsTerrainSplineTool(owner_->terrainTool_) ||
             IsTerrainSplineTool(next)))
        {
            owner_->terrainSplinePoints_.clear();
            owner_->terrainSplineTerrain_.reset();
        }

        owner_->terrainTool_ = next;

        if (owner_->views_ != nullptr)
        {
            owner_->views_->ClearTerrainAuthoringOverlay(
                "studio.primary");
        }
    }

    if (IsTerrainSplineTool(owner_->terrainTool_))
    {
        context.SameLine();
        context.MutedText(
            std::format(
                "{} pts",
                owner_->terrainSplinePoints_.size()));
    }
}

void StudioExpansionShell::DrawTerrainToolProperties(
    editor_ui::PanelContext& context)
{
    if (owner_ == nullptr ||
        owner_->session_ == nullptr)
    {
        return;
    }

    i32 tool =
        static_cast<i32>(owner_->terrainTool_);

    if (context.Combo(
            "Tool##active-terrain-tool",
            kTerrainTools,
            tool))
    {
        const auto next =
            static_cast<StudioTerrainAuthoringTool>(tool);

        if (owner_->terrainTool_ != next &&
            (IsTerrainSplineTool(owner_->terrainTool_) ||
             IsTerrainSplineTool(next)))
        {
            owner_->terrainSplinePoints_.clear();
            owner_->terrainSplineTerrain_.reset();
        }

        owner_->terrainTool_ = next;

        if (owner_->views_ != nullptr)
        {
            owner_->views_->ClearTerrainAuthoringOverlay(
                "studio.primary");
        }
    }

    switch (owner_->terrainTool_)
    {
    case StudioTerrainAuthoringTool::Select:
        context.MutedText(
            "Selection mode has no brush parameters. Click terrain to select and inspect it.");
        return;

    case StudioTerrainAuthoringTool::Raise:
    case StudioTerrainAuthoringTool::Lower:
    case StudioTerrainAuthoringTool::Protection:
    case StudioTerrainAuthoringTool::Drainage:
    case StudioTerrainAuthoringTool::Material:
        static_cast<void>(context.InputDouble(
            "Inner Radius (m)##active-terrain-inner",
            owner_->terrainBrushInnerRadiusMeters_));
        static_cast<void>(context.InputDouble(
            "Outer Radius (m)##active-terrain-outer",
            owner_->terrainBrushOuterRadiusMeters_));

        owner_->terrainBrushInnerRadiusMeters_ =
            std::max(0.0, owner_->terrainBrushInnerRadiusMeters_);
        owner_->terrainBrushOuterRadiusMeters_ =
            std::max(
                owner_->terrainBrushInnerRadiusMeters_,
                owner_->terrainBrushOuterRadiusMeters_);

        if (owner_->terrainTool_ ==
                StudioTerrainAuthoringTool::Raise ||
            owner_->terrainTool_ ==
                StudioTerrainAuthoringTool::Lower)
        {
            static_cast<void>(context.InputDouble(
                "Height Delta (m)##active-terrain-height",
                owner_->terrainBrushHeightMeters_));
        }
        else if (owner_->terrainTool_ ==
                 StudioTerrainAuthoringTool::Protection)
        {
            static_cast<void>(context.SliderDouble(
                "Protection##active-terrain-protection",
                owner_->terrainProtection_,
                0.0,
                1.0));
        }
        else if (owner_->terrainTool_ ==
                 StudioTerrainAuthoringTool::Drainage)
        {
            static_cast<void>(context.InputDouble(
                "Drainage Guidance##active-terrain-drainage",
                owner_->terrainDrainageGuidance_));
        }
        else
        {
            context.MutedText(
                "Geology painting currently writes the body's default bedrock material.");
        }
        return;

    case StudioTerrainAuthoringTool::Canyon:
    case StudioTerrainAuthoringTool::Ridge:
        static_cast<void>(context.InputDouble(
            "Half Width (m)##active-terrain-spline-width",
            owner_->terrainSplineHalfWidthMeters_));
        static_cast<void>(context.InputDouble(
            "Falloff (m)##active-terrain-spline-falloff",
            owner_->terrainSplineFalloffMeters_));
        static_cast<void>(context.InputDouble(
            owner_->terrainTool_ == StudioTerrainAuthoringTool::Canyon
                ? "Depth (m)##active-terrain-spline-height"
                : "Height (m)##active-terrain-spline-height",
            owner_->terrainSplineHeightMeters_));

        owner_->terrainSplineHalfWidthMeters_ =
            std::max(0.0, owner_->terrainSplineHalfWidthMeters_);
        owner_->terrainSplineFalloffMeters_ =
            std::max(0.0, owner_->terrainSplineFalloffMeters_);

        context.Text(
            std::format(
                "Control points: {} · click terrain to add; double-click to commit.",
                owner_->terrainSplinePoints_.size()));

        if (!owner_->terrainSplinePoints_.empty() &&
            context.Button(
                "Cancel Spline##active-terrain-spline-cancel"))
        {
            owner_->terrainSplinePoints_.clear();
            owner_->terrainSplineTerrain_.reset();
            if (owner_->views_ != nullptr)
            {
                owner_->views_->ClearTerrainAuthoringOverlay(
                    "studio.primary");
            }
            owner_->status_ =
                "Transient terrain spline cancelled.";
        }
        return;

    case StudioTerrainAuthoringTool::BiomePaint:
    {
        i32 operation =
            static_cast<i32>(owner_->biomePaintOperation_);
        operation = std::clamp(
            operation,
            0,
            static_cast<i32>(kBiomeOperations.size()) - 1);

        if (context.Combo(
                "Operation##active-biome-operation",
                kBiomeOperations,
                operation))
        {
            owner_->biomePaintOperation_ =
                static_cast<terrain_biome::
                    BiomeAuthoredWeightOperation>(operation);
        }

        static_cast<void>(context.InputDouble(
            "Inner Radius (m)##active-biome-inner",
            owner_->biomeBrushInnerRadiusMeters_));
        static_cast<void>(context.InputDouble(
            "Outer Radius (m)##active-biome-outer",
            owner_->biomeBrushOuterRadiusMeters_));
        static_cast<void>(context.SliderDouble(
            "Strength##active-biome-strength",
            owner_->biomeBrushValue_,
            0.0,
            1.0));
        static_cast<void>(context.SliderDouble(
            "Opacity##active-biome-opacity",
            owner_->biomeBrushOpacity_,
            0.0,
            1.0));
        static_cast<void>(context.Checkbox(
            "Automatic Placement Inspection##active-biome-auto",
            owner_->biomeAutomaticOverlay_));

        owner_->biomeBrushInnerRadiusMeters_ =
            std::max(0.0, owner_->biomeBrushInnerRadiusMeters_);
        owner_->biomeBrushOuterRadiusMeters_ =
            std::max(
                owner_->biomeBrushInnerRadiusMeters_,
                owner_->biomeBrushOuterRadiusMeters_);

        if (owner_->hoveredBiomeAuthoredWeight_.has_value())
        {
            context.Text(
                std::format(
                    "Authored weight under cursor: {:.4f}",
                    *owner_->hoveredBiomeAuthoredWeight_));
        }

        if (owner_->biomeAutomaticOverlay_)
        {
            if (owner_->hoveredBiomeAutomaticWeight_.has_value())
            {
                context.Text(
                    std::format(
                        "Automatic weight under cursor: {:.4f}",
                        *owner_->hoveredBiomeAutomaticWeight_));
            }
            else
            {
                context.MutedText(
                    "Automatic placement weight is unavailable for the current selectors/cursor.");
            }
        }
        return;
    }
    }
}

void StudioExpansionShell::DrawNavigationBand(
    editor_ui::PanelContext& context)
{
    SyncPersistentState();

    // Workspace selection and navigation are one mental model. The owner draws
    // the mode selector first; breadcrumbs, quick-create and command search
    // continue on the same row instead of reserving another strip of viewport.
    if (owner_ != nullptr)
    {
        owner_->DrawWorkspaceBand(context);
    }
    else
    {
        context.Text("Mode");
    }

    context.SameLine();
    context.MutedText("|");

    if (owner_ == nullptr ||
        owner_->session_ == nullptr ||
        !owner_->session_->World().HasWorld())
    {
        context.SameLine();
        context.MutedText("Open a world for navigation and commands.");
        return;
    }

    auto& world = owner_->session_->World();
    const auto& selection = world.Selection().Ordered();

    if (selection.size() == 1U)
    {
        const auto breadcrumbs =
            BuildSelectionBreadcrumbs(
                world.Objects(),
                selection.front());

        // Keep enough ancestry to orient the user without letting a deep
        // hierarchy consume the entire permanent row.
        const std::size_t first =
            breadcrumbs.size() > 3U
                ? breadcrumbs.size() - 3U
                : 0U;

        for (std::size_t index = first;
             index < breadcrumbs.size();
             ++index)
        {
            context.SameLine();
            if (index != first)
            {
                context.MutedText(">");
                context.SameLine();
            }

            const auto& breadcrumb = breadcrumbs[index];
            std::string label = breadcrumb.label;
            label += "##breadcrumb-";
            label += breadcrumb.id.ToString();

            if (context.Button(label))
            {
                const std::array selected{breadcrumb.id};
                world.Selection().Set(
                    std::span<const scene::ObjectId>(selected));
            }
        }
    }

    context.SameLine();
    if (context.Button(
            quickCreateOpen_
                ? "Close Add##quick-create-toggle"
                : "+ Add##quick-create-toggle"))
    {
        quickCreateOpen_ = !quickCreateOpen_;
    }

    context.SameLine();
    if (context.Button(
            commandSearchOpen_
                ? "Close Commands##command-palette-toggle"
                : "Commands##command-palette-toggle"))
    {
        commandSearchOpen_ = !commandSearchOpen_;
        if (!commandSearchOpen_)
        {
            commandQuery_.clear();
        }
    }

    auto& registry = world.CommandRegistry();
    const auto palette =
        BuildCommandPalette(registry.Catalog());

    if (commandSearchOpen_)
    {
        context.SameLine();
        static_cast<void>(
            context.InputText(
                "##studio-command-palette-query",
                commandQuery_));

        const auto matches =
            SearchCommandPalette(
                palette,
                commandQuery_,
                8U);

        std::size_t shown = 0U;
        for (const auto& entry : matches)
        {
            if (entry.requiresArguments)
            {
                continue;
            }

            const auto enablement =
                registry.Enablement(entry.command);
            if (!enablement.enabled)
            {
                continue;
            }

            context.SameLine();
            std::string label = entry.label;
            label += "##palette-";
            label += entry.command.ToString();
            if (context.Button(label))
            {
                try
                {
                    registry.Invoke(entry.command);
                    owner_->status_.clear();
                    commandSearchOpen_ = false;
                    commandQuery_.clear();
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }

            if (++shown >= 2U)
            {
                break;
            }
        }
    }

    if (quickCreateOpen_)
    {
        DrawBuiltInQuickCreate(context);

        std::size_t shown = 0U;
        for (const auto& entry : palette)
        {
            if (entry.requiresArguments ||
                !IsQuickCreateLabel(entry.label))
            {
                continue;
            }

            const auto enablement =
                registry.Enablement(entry.command);
            if (!enablement.enabled)
            {
                continue;
            }

            context.SameLine();
            std::string label = entry.label;
            label += "##quick-create-";
            label += entry.command.ToString();
            if (context.Button(label))
            {
                try
                {
                    registry.Invoke(entry.command);
                    owner_->status_.clear();
                    quickCreateOpen_ = false;
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }

            if (++shown >= 2U)
            {
                break;
            }
        }

        DrawContributions(
            context,
            StudioContributionSurface::QuickCreate);
    }

    DrawContributions(
        context,
        StudioContributionSurface::WorkspaceToolbar);
}

void StudioExpansionShell::DrawViewportBand(
    editor_ui::PanelContext& context)
{
    SyncPersistentState();

    // Selection-driven authoring is the primary content of row two. Generic
    // viewport/gizmo state follows it compactly rather than living in a fourth
    // permanent toolbar. Terrain gets one selector here; only its active
    // parameters are disclosed in Properties.
    if (owner_ != nullptr)
    {
        if (TerrainContextRelevant())
        {
            DrawTerrainContext(context);
        }
        else
        {
            owner_->DrawContextBand(context);
        }
    }
    else
    {
        context.Text("Context");
    }

    context.SameLine();
    context.MutedText("|");
    context.SameLine();
    context.Text("View");

    // Auto mode follows focus, so the target does not need permanent toolbar
    // chrome. Only surface a compact cue when the user explicitly pins the
    // control target in Properties.
    if (viewportControlMode_ != 0)
    {
        context.SameLine();
        context.MutedText(
            viewportControlMode_ == 2
                ? "Pinned Map"
                : "Pinned P");
    }

    if (ViewportControlsRelevant())
    {
        const std::string_view id = SelectedViewportId();
        const auto* target = owner_->session_->Viewports().Find(id);

        context.SameLine();
        if (target != nullptr &&
            target->mode == studio_session::ViewportMode::Debug)
        {
            const auto catalog = terrain_debug::FieldCatalog();
            std::vector<std::string_view> names;
            names.reserve(catalog.size());

            i32 selected = 0;
            const auto field = owner_->views_->DebugField(id);
            for (std::size_t index = 0U; index < catalog.size(); ++index)
            {
                names.push_back(catalog[index].name);
                if (catalog[index].field == field)
                {
                    selected = static_cast<i32>(index);
                }
            }

            if (!names.empty() &&
                context.Combo(
                    "##viewport-debug-field-compact",
                    names,
                    selected))
            {
                selected = std::clamp<i32>(
                    selected,
                    0,
                    static_cast<i32>(catalog.size()) - 1);
                owner_->views_->SetDebugField(
                    id,
                    catalog[static_cast<std::size_t>(selected)].field);
            }
        }
        else
        {
            i32 surface = static_cast<i32>(
                owner_->views_->SurfaceDebugMode(id));
            if (context.Combo(
                    "##surface-debug-mode",
                    kSurfaceViews,
                    surface))
            {
                surface = std::clamp(surface, 0, 3);
                owner_->views_->SetSurfaceDebugMode(
                    id,
                    static_cast<lighting::SurfaceDebugMode>(surface));
            }
        }
    }

    context.SameLine();

    static constexpr std::array<std::string_view, 4>
        kTools{"Select", "Move", "Rotate", "Scale"};
    i32 tool = static_cast<i32>(viewportState_.gizmo.tool);
    if (context.SegmentedControl(
            "viewport-gizmo-tool",
            kTools,
            tool))
    {
        viewportState_.gizmo.tool =
            static_cast<GizmoTool>(tool);
    }

    context.SameLine();
    static constexpr std::array<std::string_view, 4>
        kSnapModes{"Off", "Grid", "Surface", "Both"};
    i32 snapMode =
        (viewportState_.gizmo.translationSnap ? 1 : 0) |
        (viewportState_.gizmo.surfaceSnap ? 2 : 0);
    if (context.Combo(
            "##gizmo-snap-mode",
            kSnapModes,
            snapMode))
    {
        viewportState_.gizmo.translationSnap =
            (snapMode & 1) != 0;
        viewportState_.gizmo.surfaceSnap =
            (snapMode & 2) != 0;
    }

    DrawContributions(
        context,
        StudioContributionSurface::ContextToolbar);
}
} // namespace orbit::studio_ui