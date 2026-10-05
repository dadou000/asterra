#include <orbit/studio_ui/StudioExpansionShell.hpp>

#include <orbit/content/ContentService.hpp>
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
#include <cmath>
#include <exception>
#include <format>
#include <functional>
#include <limits>
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

[[nodiscard]] bool IsQuickCreateCatalogEntry(
    const commands::CommandCatalogEntry& entry) noexcept
{
    if (IsQuickCreateLabel(entry.name))
    {
        return true;
    }

    return std::ranges::any_of(
        entry.presentationSurfaces,
        [](const std::string& surface)
        {
            return surface == kStudioQuickCreateCommandSurface;
        });
}

[[nodiscard]] std::string CommandPaletteSecondaryText(
    const CommandPaletteEntry& entry)
{
    std::string secondary = entry.category;
    if (!entry.description.empty())
    {
        if (!secondary.empty())
        {
            secondary += " · ";
        }
        secondary += entry.description;
    }

    for (char& value : secondary)
    {
        if (value == '\n' ||
            value == '\r' ||
            value == '\t')
        {
            value = ' ';
        }
    }

    constexpr std::size_t kMaxSecondaryCharacters = 96U;
    if (secondary.size() > kMaxSecondaryCharacters)
    {
        secondary.resize(kMaxSecondaryCharacters - 3U);
        secondary += "...";
    }

    return secondary;
}

[[nodiscard]] bool IsTerrainSplineTool(
    const StudioTerrainAuthoringTool tool) noexcept
{
    return tool == StudioTerrainAuthoringTool::Canyon ||
        tool == StudioTerrainAuthoringTool::Ridge;
}

[[nodiscard]] const char* ViewportModeLabel(
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

bool StudioExpansionShell::DrawContributions(
    editor_ui::PanelContext& context,
    const StudioContributionSurface surface,
    const bool responsiveOverflow,
    const bool verticalList)
{
    if (owner_ == nullptr ||
        owner_->session_ == nullptr ||
        !owner_->session_->World().HasWorld())
    {
        return false;
    }

    auto& commands =
        owner_->session_->World().CommandRegistry();

    bool invokedSuccessfully = false;
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
                [this, command, &invokedSuccessfully]
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
                        invokedSuccessfully = true;
                    }
                    catch (const std::exception& exception)
                    {
                        owner_->status_ = exception.what();
                    }
                }
        });
    }

    if (actions.empty())
    {
        return false;
    }

    if (verticalList)
    {
        static_cast<void>(context.ActionList(actions));
        return invokedSuccessfully;
    }

    context.SameLine();

    // Keep built-in shell controls stable. Only extensible contributions
    // collapse when the current row runs short on horizontal space.
    // ContentAvailable() is evaluated after all high-priority controls have
    // drawn, so this adapts to both window width and the active context.
    constexpr f32 kInlineContributionReserve = 300.0F;
    const bool overflow =
        responsiveOverflow &&
        context.ContentAvailable().width <
            kInlineContributionReserve * editor_ui::CurrentUiScale();

    if (!overflow)
    {
        context.Toolbar(actions);
        return invokedSuccessfully;
    }

    const bool workspaceSurface =
        surface == StudioContributionSurface::WorkspaceToolbar;
    const char* overflowButton =
        workspaceSurface
            ? "…##studio-workspace-toolbar-overflow"
            : "…##studio-context-toolbar-overflow";
    const char* overflowMenu =
        workspaceSurface
            ? "studio-workspace-toolbar-overflow-menu"
            : "studio-context-toolbar-overflow-menu";

    const bool openOverflow =
        context.Button(overflowButton);
    context.ContextMenu(
        overflowMenu,
        actions,
        openOverflow);
    return invokedSuccessfully;
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

bool StudioExpansionShell::DrawBuiltInQuickCreate(
    editor_ui::PanelContext& context)
{
    if (owner_ == nullptr)
    {
        return false;
    }

    bool created = false;

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
            [this, viewportId, &created]
            {
                try
                {
                    owner_->CreateLocalLightAtViewport(
                        viewportId,
                        false);
                    created = true;
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
            [this, viewportId, &created]
            {
                try
                {
                    owner_->CreateLocalLightAtViewport(
                        viewportId,
                        true);
                    created = true;
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
            [this, viewportId, &created]
            {
                try
                {
                    owner_->CreateVisibilityProxyAtViewport(
                        viewportId,
                        false);
                    created = true;
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
            [this, viewportId, &created]
            {
                try
                {
                    owner_->CreateVisibilityProxyAtViewport(
                        viewportId,
                        true);
                    created = true;
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }
    });

    static_cast<void>(context.ActionList(actions));
    return created;
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

    switch (viewportState_.gizmo.tool)
    {
    case GizmoTool::Select:
        break;

    case GizmoTool::Translate:
    {
        static constexpr std::array<std::string_view, 4>
            kTranslationSnapModes{"Off", "Grid", "Surface", "Both"};
        i32 snapMode =
            (viewportState_.gizmo.translationSnap ? 1 : 0) |
            (viewportState_.gizmo.surfaceSnap ? 2 : 0);
        if (context.Combo(
                "Snap##viewport-translation-snap-properties",
                kTranslationSnapModes,
                snapMode))
        {
            viewportState_.gizmo.translationSnap =
                (snapMode & 1) != 0;
            viewportState_.gizmo.surfaceSnap =
                (snapMode & 2) != 0;
        }
        if (viewportState_.gizmo.translationSnap)
        {
            static_cast<void>(context.InputDouble(
                "Grid Step (m)##viewport-translation-snap-step",
                viewportState_.gizmo.translationSnapMeters));
            viewportState_.gizmo.translationSnapMeters =
                std::max(0.001, viewportState_.gizmo.translationSnapMeters);
        }
        break;
    }

    case GizmoTool::Rotate:
    {
        static constexpr std::array<std::string_view, 2>
            kRotationSnapModes{"Off", "Angle"};
        i32 snapMode = viewportState_.gizmo.rotationSnap ? 1 : 0;
        if (context.Combo(
                "Snap##viewport-rotation-snap-properties",
                kRotationSnapModes,
                snapMode))
        {
            viewportState_.gizmo.rotationSnap = snapMode != 0;
        }
        if (viewportState_.gizmo.rotationSnap)
        {
            static_cast<void>(context.InputDouble(
                "Angle Step (deg)##viewport-rotation-snap-step",
                viewportState_.gizmo.rotationSnapDegrees));
            viewportState_.gizmo.rotationSnapDegrees =
                std::max(0.001, viewportState_.gizmo.rotationSnapDegrees);
        }
        break;
    }

    case GizmoTool::Scale:
    {
        static constexpr std::array<std::string_view, 2>
            kScaleSnapModes{"Off", "Step"};
        i32 snapMode = viewportState_.gizmo.scaleSnap ? 1 : 0;
        if (context.Combo(
                "Snap##viewport-scale-snap-properties",
                kScaleSnapModes,
                snapMode))
        {
            viewportState_.gizmo.scaleSnap = snapMode != 0;
        }
        if (viewportState_.gizmo.scaleSnap)
        {
            static_cast<void>(context.InputDouble(
                "Scale Step##viewport-scale-snap-step",
                viewportState_.gizmo.scaleSnapStep));
            viewportState_.gizmo.scaleSnapStep =
                std::max(0.001, viewportState_.gizmo.scaleSnapStep);
        }
        break;
    }
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
            ViewportModeLabel(target->mode)));

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

    {
        bool textHud = owner_->views_->TextDiagnosticsHud(id);
        if (context.Checkbox(
                "Text readout (position, heights, biome)##diag-text",
                textHud))
        {
            owner_->views_->SetTextDiagnosticsHud(id, textHud);
        }
    }

    if (target->mode == studio_session::ViewportMode::Perspective)
    {
        if (context.Section("Terrain layers##diag-layers", true))
        {
            auto layers = owner_->views_->TerrainLayers(id);
            bool layersChanged = false;

            layersChanged =
                context.Checkbox(
                    "Near-field terrain##layer-production",
                    layers.productionSurface) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Full clipmap renderer (ground to orbit, no globe)##layer-full-clipmap",
                    layers.fullClipmap) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Orbital globe patches##layer-macro",
                    layers.macroGlobe) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Ocean##layer-ocean",
                    layers.ocean) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Surface effects##layer-effects",
                    layers.surfaceEffects) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Clouds##layer-clouds",
                    layers.clouds) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Cloud light volume##layer-cloud-light-volume",
                    layers.cloudLightVolume) || layersChanged;
            context.MutedText("Bypass a frame stage (for bisecting artefacts):");
            layersChanged =
                context.Checkbox(
                    "Bypass cloud shadow##bypass-cloud-shadow",
                    layers.bypassCloudShadow) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Bypass indirect lighting (final gather + reflections)##bypass-indirect",
                    layers.bypassIndirectLighting) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Bypass hybrid reflections only##bypass-hybrid-reflections",
                    layers.bypassHybridReflections) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Bypass radiance cache fallback only##bypass-radiance-cache",
                    layers.bypassRadianceCache) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Bypass near-field water##bypass-near-water",
                    layers.bypassNearFieldWater) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Bypass atmosphere (and clouds)##bypass-atmosphere",
                    layers.bypassAtmosphere) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Indirect lighting coverage view##indirect-coverage",
                    layers.indirectCoverageView) || layersChanged;
            f64 volumeDebugAltitude = static_cast<f64>(layers.cloudVolumeDebugAltitude);
            if (context.SliderDouble(
                    "Light volume debug slice (m, 0 = off)##layer-cloud-volume-debug",
                    volumeDebugAltitude,
                    0.0,
                    20000.0))
            {
                layers.cloudVolumeDebugAltitude = static_cast<f32>(volumeDebugAltitude);
                layersChanged = true;
            }
            f64 godrayStrength = static_cast<f64>(layers.cloudGodrayStrength);
            if (context.SliderDouble(
                    "God-ray strength##layer-cloud-godrays",
                    godrayStrength,
                    0.0,
                    2.0))
            {
                layers.cloudGodrayStrength = static_cast<f32>(godrayStrength);
                layersChanged = true;
            }

            f64 bias = static_cast<f64>(layers.lodBiasStops);
            if (context.SliderDouble(
                    "LOD bias (stops)##layer-lod-bias",
                    bias,
                    -4.0,
                    4.0))
            {
                layers.lodBiasStops = static_cast<f32>(bias);
                layersChanged = true;
            }
            context.MutedText(
                "+ keeps richer terrain longer and doubles orbital patch detail; - is coarser and cheaper.");

            layersChanged =
                context.Checkbox(
                    "Dynamic clipmap levels##layer-dynamic-clipmaps",
                    layers.dynamicClipmaps) || layersChanged;
            f64 pixelsPerVertex = static_cast<f64>(layers.clipmapPixelsPerVertex);
            if (context.SliderDouble(
                    "Pixels per vertex##layer-clipmap-ppv",
                    pixelsPerVertex,
                    0.25,
                    16.0))
            {
                layers.clipmapPixelsPerVertex = static_cast<f32>(pixelsPerVertex);
                layersChanged = true;
            }
            f64 fadeSeconds = static_cast<f64>(layers.clipmapFadeSeconds);
            if (context.SliderDouble(
                    "Level fade (s)##layer-clipmap-fade",
                    fadeSeconds,
                    0.0,
                    3.0))
            {
                layers.clipmapFadeSeconds = static_cast<f32>(fadeSeconds);
                layersChanged = true;
            }
            layersChanged =
                context.Checkbox(
                    "EXPERIMENT: distance-banded levels##layer-distance-bands",
                    layers.experimentalDistanceBands) || layersChanged;
            f64 bandScale = static_cast<f64>(layers.clipmapBandScale);
            if (context.SliderDouble(
                    "Band distance scale##layer-band-scale",
                    bandScale,
                    0.25,
                    4.0))
            {
                layers.clipmapBandScale = static_cast<f32>(bandScale);
                layersChanged = true;
            }
            if (layers.experimentalDistanceBands)
            {
                // One rendering distance per clipmap level: level k is drawn
                // between the previous level's distance and this one.
                for (std::size_t edgeIndex = 0U;
                     edgeIndex < layers.clipmapBandEdgesMeters.size() &&
                     layers.clipmapBandEdgesMeters[edgeIndex] > 0.0F;
                     ++edgeIndex)
                {
                    f64 edge = static_cast<f64>(layers.clipmapBandEdgesMeters[edgeIndex]);
                    const f64 lower = edgeIndex == 0U
                        ? 1.0
                        : static_cast<f64>(layers.clipmapBandEdgesMeters[edgeIndex - 1U]) * 1.05;
                    const std::string label = std::format(
                        "Level {} reaches (m)##layer-band-edge-{}", edgeIndex, edgeIndex);
                    if (context.InputDouble(label, edge))
                    {
                        // Keep the edges increasing; the next one is pushed out if needed.
                        edge = std::max(edge, lower);
                        layers.clipmapBandEdgesMeters[edgeIndex] = static_cast<f32>(edge);
                        for (std::size_t next = edgeIndex + 1U;
                             next < layers.clipmapBandEdgesMeters.size() &&
                             layers.clipmapBandEdgesMeters[next] > 0.0F;
                             ++next)
                        {
                            layers.clipmapBandEdgesMeters[next] = std::max(
                                layers.clipmapBandEdgesMeters[next],
                                layers.clipmapBandEdgesMeters[next - 1U] * 1.05F);
                        }
                        layersChanged = true;
                    }
                }
            }
            context.MutedText(
                "Draws clipmap level k only where the camera's distance to the terrain is in its band "
                "(default 0-100 m, 100-500 m, 500-2 km, 2-10 km, ...), cross-fading neighbours so the rings "
                "resize with the camera. Needs the full clipmap renderer. Edit the bands over RPC "
                "(clipmap_band_edges_meters).");
            context.MutedText(
                "Dynamic levels draw only the clipmap levels the camera can use: fine levels vanish "
                "as you rise, coarse ones when ground is not in view. Lower pixels-per-vertex keeps finer levels.");

            if (context.Button("Reset layers##layer-reset"))
            {
                layers = {};
                layersChanged = true;
            }

            if (layersChanged)
            {
                owner_->views_->SetTerrainLayers(id, layers);
            }
        }

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
            "Active clipmap rings##diag-clipmap-rings",
            diagnostics.clipmapRings);
        toggle(
            "Tint terrain by clipmap level##diag-clipmap-levels",
            diagnostics.clipmapLevels);
        toggle(
            "Clipmap sample health (bad elevation/morph/slope)##diag-clipmap-sample-health",
            diagnostics.clipmapSampleHealth);
        toggle(
            "Clipmap hole / fade view (why vertices are culled)##diag-clipmap-hole-view",
            diagnostics.clipmapHoleView);
        toggle(
            "Clipmap projected position view (clipped vertices)##diag-clipmap-projection-view",
            diagnostics.clipmapProjectionView);
        toggle(
            "Clipmap shading view (bad interpolated inputs)##diag-clipmap-shading-view",
            diagnostics.clipmapShadingView);
        toggle(
            "Clipmap wireframe##diag-clipmap-wireframe",
            diagnostics.clipmapWireframe);
        toggle(
            "Freeze clipmaps##diag-clipmap-freeze",
            diagnostics.clipmapFreeze);
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

        // Breadcrumbs are the flexible part of row 1. Preserve enough room
        // for the two high-frequency actions that follow (+ Add and Commands),
        // then retain the selected object and as many nearest parents as fit.
        constexpr std::size_t kMaximumVisibleBreadcrumbs = 3U;
        const f32 scale = editor_ui::CurrentUiScale();
        const f32 breadcrumbBudget = std::max(
            0.0F,
            context.ContentAvailable().width - 300.0F * scale);

        const auto estimatedWidth =
            [scale, &breadcrumbs](
                const std::size_t first) noexcept
            {
                f32 width = 0.0F;
                for (std::size_t index = first;
                     index < breadcrumbs.size();
                     ++index)
                {
                    width +=
                        static_cast<f32>(breadcrumbs[index].label.size()) *
                            8.0F * scale +
                        30.0F * scale;
                    if (index != first)
                    {
                        width += 22.0F * scale;
                    }
                }
                return width;
            };

        std::size_t first =
            breadcrumbs.size() > kMaximumVisibleBreadcrumbs
                ? breadcrumbs.size() - kMaximumVisibleBreadcrumbs
                : 0U;
        while (first + 1U < breadcrumbs.size() &&
               estimatedWidth(first) > breadcrumbBudget)
        {
            ++first;
        }

        if (first > 0U)
        {
            std::vector<editor_ui::ActionPresentation> hiddenAncestors;
            hiddenAncestors.reserve(first);
            for (std::size_t index = 0U; index < first; ++index)
            {
                const auto breadcrumb = breadcrumbs[index];
                hiddenAncestors.push_back({
                    .label = breadcrumb.label,
                    .invoke =
                        [&world, id = breadcrumb.id]
                        {
                            const std::array selected{id};
                            world.Selection().Set(
                                std::span<const scene::ObjectId>(selected));
                        }
                });
            }

            context.SameLine();
            const bool openHidden =
                context.Button("…##breadcrumb-overflow");
            context.ContextMenu(
                "breadcrumb-overflow-menu",
                hiddenAncestors,
                openHidden);
        }

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
    const bool openQuickCreate =
        context.Button("+ Add##quick-create-toggle");

    context.SameLine();
    const f32 commandHintThreshold =
        230.0F * editor_ui::CurrentUiScale();
    const std::string_view commandButtonLabel =
        context.ContentAvailable().width >= commandHintThreshold
            ? "Commands  /##command-palette-toggle"
            : "Commands##command-palette-toggle";
    const bool openCommandPalette =
        context.Button(commandButtonLabel) ||
        std::exchange(commandPaletteOpenRequested_, false);
    if (openCommandPalette)
    {
        commandQuery_.clear();
        commandPaletteSelection_ = 0;
    }

    auto& registry = world.CommandRegistry();
    const auto commandCatalog = registry.Catalog();
    const auto palette =
        BuildCommandPalette(commandCatalog);

    const f32 commandPaletteWidth =
        520.0F * editor_ui::CurrentUiScale();
    if (context.BeginPopup(
            "studio-command-palette-popup",
            openCommandPalette,
            {.width = commandPaletteWidth, .height = 0.0F}))
    {
        context.Text("Commands");
        if (openCommandPalette)
        {
            context.FocusNextItem();
        }
        if (context.InputText(
                "##studio-command-palette-query",
                commandQuery_))
        {
            commandPaletteSelection_ = 0;
        }
        context.Separator();

        const auto matches =
            SearchCommandPalette(
                palette,
                commandQuery_,
                palette.size());

        std::vector<CommandPaletteEntry> visibleMatches;
        visibleMatches.reserve(8U);
        for (const auto& entry : matches)
        {
            if (entry.requiresArguments ||
                !registry.Enablement(entry.command).enabled)
            {
                continue;
            }

            visibleMatches.push_back(entry);
            if (visibleMatches.size() >= 8U)
            {
                break;
            }
        }

        const auto invokeEntry =
            [&](const CommandPaletteEntry& entry)
            {
                try
                {
                    registry.Invoke(entry.command);
                    owner_->status_.clear();
                    commandQuery_.clear();
                    commandPaletteSelection_ = 0;
                    context.CloseCurrentPopup();
                    return true;
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                    return false;
                }
            };

        if (context.KeyPressed(editor_ui::UiKey::Escape))
        {
            commandQuery_.clear();
            commandPaletteSelection_ = 0;
            context.CloseCurrentPopup();
        }
        else if (visibleMatches.empty())
        {
            commandPaletteSelection_ = 0;
            context.MutedText("No matching enabled commands.");
        }
        else
        {
            const i32 visibleCount =
                static_cast<i32>(visibleMatches.size());
            commandPaletteSelection_ = std::clamp(
                commandPaletteSelection_,
                0,
                visibleCount - 1);

            if (context.KeyPressed(
                    editor_ui::UiKey::Down,
                    true))
            {
                commandPaletteSelection_ =
                    (commandPaletteSelection_ + 1) % visibleCount;
            }
            if (context.KeyPressed(
                    editor_ui::UiKey::Up,
                    true))
            {
                commandPaletteSelection_ =
                    (commandPaletteSelection_ + visibleCount - 1) %
                    visibleCount;
            }

            bool invoked = false;
            for (i32 index = 0; index < visibleCount; ++index)
            {
                const auto& entry =
                    visibleMatches[static_cast<std::size_t>(index)];
                std::string label = entry.label;
                const std::string secondary =
                    CommandPaletteSecondaryText(entry);
                if (!secondary.empty())
                {
                    label += "\n";
                    label += secondary;
                }
                label += "##palette-";
                label += entry.command.ToString();
                if (context.Selectable(
                        label,
                        index == commandPaletteSelection_))
                {
                    commandPaletteSelection_ = index;
                    invoked = invokeEntry(entry);
                    if (invoked)
                    {
                        break;
                    }
                }
            }

            if (!invoked &&
                context.KeyPressed(editor_ui::UiKey::Enter))
            {
                static_cast<void>(
                    invokeEntry(
                        visibleMatches[static_cast<std::size_t>(
                            commandPaletteSelection_)]));
            }
        }

        context.EndPopup();
    }

    bool openQuickCreateBrowser = false;
    const f32 quickCreateWidth =
        360.0F * editor_ui::CurrentUiScale();
    if (context.BeginPopup(
            "studio-quick-create-popup",
            openQuickCreate,
            {.width = quickCreateWidth, .height = 0.0F}))
    {
        context.Text("Add");

        bool closeQuickCreate = false;

        struct RankedQuickCreateAction
        {
            i32 priority{0};
            editor_ui::ActionPresentation action;
        };

        std::vector<RankedQuickCreateAction> rankedCommandActions;
        rankedCommandActions.reserve(8U);
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

            const i32 priority =
                owner_->QuickCreateCommandPriority(entry.category);
            if (priority <= 0)
            {
                continue;
            }

            const auto command = entry.command;
            std::string label = entry.label;
            label += "##quick-create-";
            label += command.ToString();
            rankedCommandActions.push_back({
                .priority = priority,
                .action = {
                    .label = std::move(label),
                    .invoke =
                        [&registry, &closeQuickCreate, command, this]
                        {
                            try
                            {
                                registry.Invoke(command);
                                owner_->status_.clear();
                                closeQuickCreate = true;
                            }
                            catch (const std::exception& exception)
                            {
                                owner_->status_ = exception.what();
                            }
                        }
                }
            });
        }

        std::stable_sort(
            rankedCommandActions.begin(),
            rankedCommandActions.end(),
            [](const RankedQuickCreateAction& left,
               const RankedQuickCreateAction& right)
            {
                return left.priority > right.priority;
            });

        std::vector<editor_ui::ActionPresentation> contextualCommandActions;
        std::vector<editor_ui::ActionPresentation> fallbackCommandActions;
        contextualCommandActions.reserve(4U);
        fallbackCommandActions.reserve(3U);

        for (auto& ranked : rankedCommandActions)
        {
            if (ranked.priority > 1)
            {
                if (contextualCommandActions.size() < 4U)
                {
                    contextualCommandActions.push_back(
                        std::move(ranked.action));
                }
            }
            else if (fallbackCommandActions.size() < 3U)
            {
                fallbackCommandActions.push_back(
                    std::move(ranked.action));
            }
        }

        bool drewQuickCreateSection = false;
        const auto drawCommandActions =
            [&](const std::string_view heading,
                const std::vector<editor_ui::ActionPresentation>& actions)
            {
                if (actions.empty())
                {
                    return;
                }

                if (drewQuickCreateSection)
                {
                    context.Separator();
                }
                context.MutedText(heading);
                static_cast<void>(context.ActionList(actions));
                drewQuickCreateSection = true;
            };

        const auto drawContextualCommands = [&]
        {
            drawCommandActions(
                world.Selection().Ordered().empty()
                    ? std::string_view{"For Workspace"}
                    : std::string_view{"For Selection"},
                contextualCommandActions);
        };

        const auto drawFallbackCommands = [&]
        {
            drawCommandActions(
                "More",
                fallbackCommandActions);
        };

        const auto drawViewportActions = [&]
        {
            if (!owner_->ShowViewportQuickCreate())
            {
                return;
            }

            if (drewQuickCreateSection)
            {
                context.Separator();
            }
            context.MutedText("Viewport");
            closeQuickCreate =
                DrawBuiltInQuickCreate(context) ||
                closeQuickCreate;
            drewQuickCreateSection = true;
        };

        const bool hasSelection =
            !world.Selection().Ordered().empty();
        if (owner_->PreferCommandQuickCreate() || hasSelection)
        {
            drawContextualCommands();
            drawViewportActions();
        }
        else
        {
            drawViewportActions();
            drawContextualCommands();
        }
        drawFallbackCommands();

        const auto pluginCatalog =
            GlobalStudioUiContributions().Catalog(
                StudioContributionSurface::QuickCreate);

        struct RankedPluginQuickCreateAction
        {
            i32 priority{0};
            editor_ui::ActionPresentation action;
        };

        std::vector<RankedPluginQuickCreateAction> rankedPluginActions;
        rankedPluginActions.reserve(pluginCatalog.size());

        for (const auto& contribution : pluginCatalog)
        {
            if (contribution.kind != StudioContributionKind::Command ||
                !contribution.command.IsValid())
            {
                continue;
            }

            const auto enablement =
                registry.Enablement(contribution.command);
            if (!enablement.enabled)
            {
                continue;
            }

            std::string_view category = contribution.category;
            if (category.empty())
            {
                const auto commandEntry =
                    std::ranges::find_if(
                        palette,
                        [&contribution](const CommandPaletteEntry& entry)
                        {
                            return entry.command == contribution.command;
                        });
                if (commandEntry != palette.end())
                {
                    category = commandEntry->category;
                }
            }

            const i32 priority =
                owner_->QuickCreateCommandPriority(category);
            if (priority <= 0)
            {
                continue;
            }

            const auto command = contribution.command;
            std::string label = contribution.label;
            label += "##quick-create-extension-";
            label += contribution.id;

            rankedPluginActions.push_back({
                .priority = priority,
                .action = {
                    .label = std::move(label),
                    .invoke =
                        [this, &closeQuickCreate, command]
                        {
                            try
                            {
                                owner_->session_->World().CommandRegistry().Invoke(
                                    command);
                                owner_->status_.clear();
                                closeQuickCreate = true;
                            }
                            catch (const std::exception& exception)
                            {
                                owner_->status_ = exception.what();
                            }
                        }
                }
            });
        }

        std::stable_sort(
            rankedPluginActions.begin(),
            rankedPluginActions.end(),
            [](const RankedPluginQuickCreateAction& left,
               const RankedPluginQuickCreateAction& right)
            {
                return left.priority > right.priority;
            });

        std::vector<editor_ui::ActionPresentation> pluginContextualActions;
        std::vector<editor_ui::ActionPresentation> pluginFallbackActions;
        pluginContextualActions.reserve(4U);
        pluginFallbackActions.reserve(3U);

        for (auto& ranked : rankedPluginActions)
        {
            if (ranked.priority > 1)
            {
                if (pluginContextualActions.size() < 4U)
                {
                    pluginContextualActions.push_back(
                        std::move(ranked.action));
                }
            }
            else if (pluginFallbackActions.size() < 3U)
            {
                pluginFallbackActions.push_back(
                    std::move(ranked.action));
            }
        }

        drawCommandActions(
            "Extensions",
            pluginContextualActions);
        drawCommandActions(
            "More Extensions",
            pluginFallbackActions);

        if (!drewQuickCreateSection)
        {
            context.MutedText(
                "No creation actions are relevant to the current workspace and selection.");
        }

        if (drewQuickCreateSection)
        {
            context.Separator();
        }
        if (context.Button("Browse All…##quick-create-browse-all"))
        {
            quickCreateBrowseQuery_.clear();
            quickCreateBrowseSelection_ = 0;
            openQuickCreateBrowser = true;
            closeQuickCreate = true;
        }

        if (closeQuickCreate)
        {
            context.CloseCurrentPopup();
        }

        context.EndPopup();
    }

    const f32 quickCreateBrowserWidth =
        520.0F * editor_ui::CurrentUiScale();
    if (context.BeginPopup(
            "studio-quick-create-browser-popup",
            openQuickCreateBrowser,
            {.width = quickCreateBrowserWidth, .height = 0.0F}))
    {
        context.Text("Add · Browse All");
        if (openQuickCreateBrowser)
        {
            context.FocusNextItem();
        }
        if (context.InputText(
                "##studio-quick-create-browser-query",
                quickCreateBrowseQuery_))
        {
            quickCreateBrowseSelection_ = 0;
        }
        context.Separator();

        const auto clearArgumentForm = [this]
        {
            quickCreateArgumentCommand_ = {};
            quickCreateArguments_.clear();
            quickCreateArgumentEnabled_.clear();
            quickCreateIdText_.clear();
            quickCreatePickerQuery_.clear();
            quickCreatePickerSelection_.clear();
            quickCreateArgumentError_.clear();
        };

        const auto beginArgumentForm =
            [this, &registry, &clearArgumentForm](
                const commands::CommandId command)
            {
                clearArgumentForm();
                const auto* descriptor = registry.Find(command);
                if (descriptor == nullptr)
                {
                    quickCreateArgumentError_ =
                        "Command metadata is no longer available.";
                    return;
                }

                quickCreateArgumentCommand_ = command;
                for (const auto& parameter : descriptor->parameters)
                {
                    quickCreateArgumentEnabled_[parameter.name] =
                        parameter.required;

                    if (parameter.defaultValue.has_value())
                    {
                        bool compatible = false;
                        switch (parameter.kind)
                        {
                        case commands::CommandValueKind::Boolean:
                            compatible = std::holds_alternative<bool>(*parameter.defaultValue);
                            break;
                        case commands::CommandValueKind::Integer:
                            compatible = std::holds_alternative<i64>(*parameter.defaultValue);
                            break;
                        case commands::CommandValueKind::Float:
                            compatible = std::holds_alternative<f64>(*parameter.defaultValue);
                            break;
                        case commands::CommandValueKind::String:
                            compatible = std::holds_alternative<std::string>(*parameter.defaultValue);
                            break;
                        case commands::CommandValueKind::Vector3:
                            compatible = std::holds_alternative<math::Double3>(*parameter.defaultValue);
                            break;
                        case commands::CommandValueKind::ObjectId:
                            compatible = std::holds_alternative<scene::ObjectId>(*parameter.defaultValue);
                            break;
                        case commands::CommandValueKind::PropertyId:
                            compatible = std::holds_alternative<schema::PropertyId>(*parameter.defaultValue);
                            break;
                        }

                        if (compatible)
                        {
                            quickCreateArguments_[parameter.name] =
                                *parameter.defaultValue;
                            if (const auto* object =
                                    std::get_if<scene::ObjectId>(&*parameter.defaultValue))
                            {
                                quickCreateIdText_[parameter.name] =
                                    object->IsValid() ? object->ToString() : std::string{};
                            }
                            else if (const auto* property =
                                         std::get_if<schema::PropertyId>(&*parameter.defaultValue))
                            {
                                quickCreateIdText_[parameter.name] =
                                    property->IsValid() ? property->ToString() : std::string{};
                            }
                            continue;
                        }
                    }

                    if (!parameter.choices.empty())
                    {
                        quickCreateArguments_[parameter.name] =
                            parameter.choices.front().value;
                        continue;
                    }

                    switch (parameter.kind)
                    {
                    case commands::CommandValueKind::Boolean:
                        quickCreateArguments_[parameter.name] = false;
                        break;
                    case commands::CommandValueKind::Integer:
                        quickCreateArguments_[parameter.name] = i64{0};
                        break;
                    case commands::CommandValueKind::Float:
                        quickCreateArguments_[parameter.name] = f64{0.0};
                        break;
                    case commands::CommandValueKind::String:
                        quickCreateArguments_[parameter.name] = std::string{};
                        break;
                    case commands::CommandValueKind::Vector3:
                        quickCreateArguments_[parameter.name] = math::Double3{};
                        break;
                    case commands::CommandValueKind::ObjectId:
                        quickCreateArguments_[parameter.name] = scene::ObjectId{};
                        quickCreateIdText_[parameter.name] = {};
                        break;
                    case commands::CommandValueKind::PropertyId:
                        quickCreateArguments_[parameter.name] = schema::PropertyId{};
                        quickCreateIdText_[parameter.name] = {};
                        break;
                    }
                }
            };

        if (quickCreateArgumentCommand_.IsValid())
        {
            const auto* descriptor =
                registry.Find(quickCreateArgumentCommand_);
            if (descriptor == nullptr)
            {
                context.ErrorText(
                    "Command argument form failed: command metadata disappeared.");
                if (context.Button("Back##quick-create-argument-back-missing"))
                {
                    clearArgumentForm();
                }
            }
            else
            {
                if (context.Button("Back##quick-create-argument-back"))
                {
                    clearArgumentForm();
                }
                else
                {
                    context.SameLine();
                    context.Heading(descriptor->name);
                    if (!descriptor->description.empty())
                    {
                        context.MutedText(descriptor->description);
                    }

                    std::vector<const commands::CommandParameter*> formParameters;
                    formParameters.reserve(descriptor->parameters.size());
                    for (const auto& parameter : descriptor->parameters)
                    {
                        if (parameter.required)
                        {
                            formParameters.push_back(&parameter);
                        }
                    }
                    for (const auto& parameter : descriptor->parameters)
                    {
                        if (!parameter.required)
                        {
                            formParameters.push_back(&parameter);
                        }
                    }

                    const bool hasRequiredParameters = std::ranges::any_of(
                        descriptor->parameters,
                        [](const commands::CommandParameter& parameter)
                        {
                            return parameter.required;
                        });
                    if (hasRequiredParameters)
                    {
                        context.MutedText("Required inputs");
                    }

                    bool optionalSectionDrawn = false;
                    bool optionalSectionOpen = false;
                    for (const auto* parameterPointer : formParameters)
                    {
                        const auto& parameter = *parameterPointer;
                        const std::string parameterDisplayName =
                            parameter.displayName.empty()
                                ? parameter.name
                                : parameter.displayName;

                        if (!parameter.required && !optionalSectionDrawn)
                        {
                            if (hasRequiredParameters)
                            {
                                context.Separator();
                            }
                            optionalSectionOpen = context.Section(
                                "Optional inputs##quick-create-optional-inputs",
                                false);
                            optionalSectionDrawn = true;
                        }
                        if (!parameter.required && !optionalSectionOpen)
                        {
                            continue;
                        }

                        bool enabled = parameter.required ||
                            quickCreateArgumentEnabled_[parameter.name];
                        if (!parameter.required)
                        {
                            std::string optionalLabel =
                                "Set " + parameterDisplayName +
                                "##quick-create-optional-" + parameter.name;
                            if (context.Checkbox(optionalLabel, enabled))
                            {
                                quickCreateArgumentEnabled_[parameter.name] =
                                    enabled;
                            }
                        }

                        if (!enabled)
                        {
                            continue;
                        }

                        std::string visibleLabel = parameterDisplayName;
                        if (!parameter.unit.empty())
                        {
                            visibleLabel += " (" + parameter.unit + ")";
                        }
                        std::string label = visibleLabel;
                        if (parameter.required)
                        {
                            label += " *";
                        }
                        label += "##quick-create-argument-";
                        label += parameter.name;

                        auto found = quickCreateArguments_.find(parameter.name);
                        if (found == quickCreateArguments_.end())
                        {
                            continue;
                        }

                        if (!parameter.choices.empty())
                        {
                            i32 choiceIndex = 0;
                            bool currentChoiceFound = false;
                            for (std::size_t choice = 0U;
                                 choice < parameter.choices.size();
                                 ++choice)
                            {
                                if (parameter.choices[choice].value == found->second)
                                {
                                    choiceIndex = static_cast<i32>(choice);
                                    currentChoiceFound = true;
                                    break;
                                }
                            }

                            if (!currentChoiceFound)
                            {
                                choiceIndex = 0;
                                found->second = parameter.choices.front().value;
                            }

                            constexpr std::size_t kSearchableChoiceThreshold = 8U;
                            if (parameter.choices.size() <= kSearchableChoiceThreshold)
                            {
                                std::vector<std::string_view> choiceLabels;
                                choiceLabels.reserve(parameter.choices.size());
                                for (const auto& commandChoice : parameter.choices)
                                {
                                    choiceLabels.push_back(commandChoice.label);
                                }

                                if (context.Combo(label, choiceLabels, choiceIndex))
                                {
                                    found->second =
                                        parameter.choices[
                                            static_cast<std::size_t>(choiceIndex)].value;
                                }
                            }
                            else
                            {
                                std::string choiceDisplayLabel = visibleLabel;
                                if (parameter.required)
                                {
                                    choiceDisplayLabel += " *";
                                }
                                context.KeyValue(
                                    choiceDisplayLabel,
                                    parameter.choices[
                                        static_cast<std::size_t>(choiceIndex)].label);

                                const std::string pickerKey =
                                    "choice:" + parameter.name;
                                std::string chooseLabel =
                                    "Choose…##quick-create-choice-picker-" +
                                    parameter.name;
                                const bool openPicker = context.Button(chooseLabel);
                                if (openPicker)
                                {
                                    quickCreatePickerQuery_[pickerKey].clear();
                                    quickCreatePickerSelection_[pickerKey] = 0;
                                }

                                const std::string popupId =
                                    "quick-create-choice-picker-popup-" +
                                    parameter.name;
                                if (context.BeginPopup(
                                        popupId,
                                        openPicker,
                                        {.width = 420.0F * editor_ui::CurrentUiScale(),
                                         .height = 0.0F}))
                                {
                                    context.Text("Choose " + parameterDisplayName);
                                    if (openPicker)
                                    {
                                        context.FocusNextItem();
                                    }

                                    auto& query = quickCreatePickerQuery_[pickerKey];
                                    auto& resultSelection =
                                        quickCreatePickerSelection_[pickerKey];
                                    if (context.InputText(
                                            "Search##quick-create-choice-search",
                                            query))
                                    {
                                        resultSelection = 0;
                                    }
                                    if (context.KeyPressed(editor_ui::UiKey::Escape))
                                    {
                                        if (!query.empty())
                                        {
                                            query.clear();
                                            resultSelection = 0;
                                        }
                                        else
                                        {
                                            context.CloseCurrentPopup();
                                        }
                                    }
                                    context.Separator();

                                    struct RankedChoice
                                    {
                                        std::size_t index{0U};
                                        i32 score{0};
                                    };
                                    std::vector<RankedChoice> matches;
                                    for (std::size_t choice = 0U;
                                         choice < parameter.choices.size();
                                         ++choice)
                                    {
                                        const i32 score = PaletteMatchScore(
                                            parameter.choices[choice].label,
                                            query);
                                        if (!query.empty() &&
                                            score == std::numeric_limits<i32>::min())
                                        {
                                            continue;
                                        }
                                        matches.push_back({choice, score});
                                    }
                                    std::ranges::stable_sort(
                                        matches,
                                        [](const RankedChoice& left,
                                           const RankedChoice& right)
                                        {
                                            if (left.score != right.score)
                                            {
                                                return left.score > right.score;
                                            }
                                            return left.index < right.index;
                                        });

                                    if (matches.empty())
                                    {
                                        resultSelection = 0;
                                        context.MutedText("No matching choices.");
                                    }
                                    else
                                    {
                                        const i32 resultCount =
                                            static_cast<i32>(matches.size());
                                        resultSelection = std::clamp(
                                            resultSelection, 0, resultCount - 1);
                                        if (context.KeyPressed(
                                                editor_ui::UiKey::Down, true))
                                        {
                                            resultSelection =
                                                (resultSelection + 1) % resultCount;
                                        }
                                        if (context.KeyPressed(
                                                editor_ui::UiKey::Up, true))
                                        {
                                            resultSelection =
                                                (resultSelection + resultCount - 1) %
                                                resultCount;
                                        }

                                        const auto chooseChoice =
                                            [&](const std::size_t resultIndex)
                                            {
                                                found->second =
                                                    parameter.choices[resultIndex].value;
                                                query.clear();
                                                context.CloseCurrentPopup();
                                            };

                                        bool chosenByMouse = false;
                                        for (i32 index = 0;
                                             index < resultCount;
                                             ++index)
                                        {
                                            const std::size_t resultIndex =
                                                matches[static_cast<std::size_t>(index)].index;
                                            std::string option =
                                                parameter.choices[resultIndex].label +
                                                "##quick-create-choice-result-" +
                                                std::to_string(resultIndex);
                                            if (context.Selectable(
                                                    option,
                                                    parameter.choices[resultIndex].value ==
                                                        found->second))
                                            {
                                                chooseChoice(resultIndex);
                                                chosenByMouse = true;
                                                break;
                                            }
                                        }

                                        if (!chosenByMouse &&
                                            context.KeyPressed(editor_ui::UiKey::Enter))
                                        {
                                            chooseChoice(
                                                matches[static_cast<std::size_t>(
                                                    resultSelection)].index);
                                        }
                                    }
                                    context.EndPopup();
                                }
                            }
                        }
                        else
                        {
                        switch (parameter.kind)
                        {
                        case commands::CommandValueKind::Boolean:
                            if (auto* value = std::get_if<bool>(&found->second))
                            {
                                static_cast<void>(context.Checkbox(label, *value));
                            }
                            break;
                        case commands::CommandValueKind::Integer:
                            if (auto* value = std::get_if<i64>(&found->second))
                            {
                                static_cast<void>(context.InputInteger(label, *value));
                                if (parameter.minimum.has_value())
                                {
                                    const f64 bounded = std::clamp(
                                        std::ceil(*parameter.minimum),
                                        static_cast<f64>(std::numeric_limits<i64>::min()),
                                        static_cast<f64>(std::numeric_limits<i64>::max()));
                                    *value = std::max(*value, static_cast<i64>(bounded));
                                }
                                if (parameter.maximum.has_value())
                                {
                                    const f64 bounded = std::clamp(
                                        std::floor(*parameter.maximum),
                                        static_cast<f64>(std::numeric_limits<i64>::min()),
                                        static_cast<f64>(std::numeric_limits<i64>::max()));
                                    *value = std::min(*value, static_cast<i64>(bounded));
                                }
                            }
                            break;
                        case commands::CommandValueKind::Float:
                            if (auto* value = std::get_if<f64>(&found->second))
                            {
                                const bool boundedSlider =
                                    parameter.minimum.has_value() &&
                                    parameter.maximum.has_value() &&
                                    std::isfinite(*parameter.minimum) &&
                                    std::isfinite(*parameter.maximum) &&
                                    *parameter.minimum < *parameter.maximum;
                                if (boundedSlider)
                                {
                                    static_cast<void>(context.SliderDouble(
                                        label,
                                        *value,
                                        *parameter.minimum,
                                        *parameter.maximum));
                                }
                                else
                                {
                                    static_cast<void>(context.InputDouble(label, *value));
                                    if (parameter.minimum.has_value())
                                    {
                                        *value = std::max(*value, *parameter.minimum);
                                    }
                                    if (parameter.maximum.has_value())
                                    {
                                        *value = std::min(*value, *parameter.maximum);
                                    }
                                }
                            }
                            break;
                        case commands::CommandValueKind::String:
                            if (auto* value = std::get_if<std::string>(&found->second))
                            {
                                if (!parameter.assetKinds.empty() &&
                                    owner_->content_ != nullptr)
                                {
                                    auto assetMatchesParameter =
                                        [&parameter](const content::AssetRecord& asset)
                                        {
                                            const std::string_view kind =
                                                content::AssetKindName(asset.kind);
                                            return std::ranges::any_of(
                                                parameter.assetKinds,
                                                [kind](const std::string& accepted)
                                                {
                                                    return accepted == kind;
                                                });
                                        };

                                    std::string selected = value->empty()
                                        ? "None"
                                        : *value;
                                    if (!value->empty())
                                    {
                                        if (const auto* asset =
                                                owner_->content_->FindByPath(*value);
                                            asset != nullptr &&
                                            assetMatchesParameter(*asset))
                                        {
                                            selected = asset->name;
                                            selected += " · ";
                                            selected += content::AssetKindName(asset->kind);
                                        }
                                    }
                                    context.KeyValue(parameterDisplayName, selected);

                                    const std::string pickerKey =
                                        "asset:" + parameter.name;
                                    std::string chooseLabel =
                                        "Choose Asset…##quick-create-asset-picker-" +
                                        parameter.name;
                                    const bool openPicker = context.Button(chooseLabel);
                                    if (openPicker)
                                    {
                                        quickCreatePickerQuery_[pickerKey].clear();
                                        quickCreatePickerSelection_[pickerKey] = 0;
                                    }

                                    const std::string popupId =
                                        "quick-create-asset-picker-popup-" +
                                        parameter.name;
                                    if (context.BeginPopup(
                                            popupId,
                                            openPicker,
                                            {.width = 500.0F * editor_ui::CurrentUiScale(),
                                             .height = 0.0F}))
                                    {
                                        context.Text("Choose " + parameterDisplayName);
                                        if (openPicker)
                                        {
                                            context.FocusNextItem();
                                        }

                                        auto& query = quickCreatePickerQuery_[pickerKey];
                                        auto& resultSelection =
                                            quickCreatePickerSelection_[pickerKey];
                                        if (context.InputText(
                                                "Search##quick-create-asset-search",
                                                query))
                                        {
                                            resultSelection = 0;
                                        }
                                        if (context.KeyPressed(editor_ui::UiKey::Escape))
                                        {
                                            if (!query.empty())
                                            {
                                                query.clear();
                                                resultSelection = 0;
                                            }
                                            else
                                            {
                                                context.CloseCurrentPopup();
                                            }
                                        }
                                        context.Separator();

                                        std::vector<content::AssetRecord> candidates;
                                        for (auto asset : owner_->content_->Search(query))
                                        {
                                            if (assetMatchesParameter(asset))
                                            {
                                                candidates.push_back(std::move(asset));
                                            }
                                            if (candidates.size() >= 24U)
                                            {
                                                break;
                                            }
                                        }

                                        if (candidates.empty())
                                        {
                                            resultSelection = 0;
                                            context.MutedText(
                                                "No matching project assets.");
                                        }
                                        else
                                        {
                                            const i32 resultCount =
                                                static_cast<i32>(candidates.size());
                                            resultSelection = std::clamp(
                                                resultSelection, 0, resultCount - 1);
                                            if (context.KeyPressed(
                                                    editor_ui::UiKey::Down, true))
                                            {
                                                resultSelection =
                                                    (resultSelection + 1) % resultCount;
                                            }
                                            if (context.KeyPressed(
                                                    editor_ui::UiKey::Up, true))
                                            {
                                                resultSelection =
                                                    (resultSelection + resultCount - 1) %
                                                    resultCount;
                                            }

                                            const auto chooseAsset =
                                                [&](const content::AssetRecord& asset)
                                                {
                                                    *value =
                                                        asset.sourcePath.generic_string();
                                                    query.clear();
                                                    context.CloseCurrentPopup();
                                                };

                                            bool chosenByMouse = false;
                                            for (i32 index = 0;
                                                 index < resultCount;
                                                 ++index)
                                            {
                                                const auto& asset =
                                                    candidates[static_cast<std::size_t>(index)];
                                                std::string option = asset.name;
                                                option += "\n";
                                                option += content::AssetKindName(asset.kind);
                                                option += " · ";
                                                option += asset.sourcePath.generic_string();
                                                option += "##quick-create-asset-result-";
                                                option += asset.id.ToString();
                                                if (context.Selectable(
                                                        option,
                                                        *value ==
                                                            asset.sourcePath.generic_string()))
                                                {
                                                    chooseAsset(asset);
                                                    chosenByMouse = true;
                                                    break;
                                                }
                                            }

                                            if (!chosenByMouse &&
                                                context.KeyPressed(editor_ui::UiKey::Enter))
                                            {
                                                chooseAsset(
                                                    candidates[static_cast<std::size_t>(
                                                        resultSelection)]);
                                            }
                                        }
                                        context.EndPopup();
                                    }

                                    std::string advancedLabel =
                                        "Advanced asset path##quick-create-asset-path-" +
                                        parameter.name;
                                    if (context.Section(advancedLabel, false))
                                    {
                                        static_cast<void>(
                                            context.InputText(label, *value));
                                    }
                                }
                                else
                                {
                                    static_cast<void>(context.InputText(label, *value));
                                }
                            }
                            break;
                        case commands::CommandValueKind::Vector3:
                            if (auto* value = std::get_if<math::Double3>(&found->second))
                            {
                                static_cast<void>(context.InputDouble3(label, *value));
                                if (parameter.minimum.has_value())
                                {
                                    value->x = std::max(value->x, *parameter.minimum);
                                    value->y = std::max(value->y, *parameter.minimum);
                                    value->z = std::max(value->z, *parameter.minimum);
                                }
                                if (parameter.maximum.has_value())
                                {
                                    value->x = std::min(value->x, *parameter.maximum);
                                    value->y = std::min(value->y, *parameter.maximum);
                                    value->z = std::min(value->z, *parameter.maximum);
                                }
                            }
                            break;
                        case commands::CommandValueKind::ObjectId:
                        {
                            auto& idText = quickCreateIdText_[parameter.name];
                            std::string selected = "None";
                            if (const auto parsed = scene::ObjectId::Parse(idText);
                                parsed.has_value())
                            {
                                if (const auto record = world.Objects().Find(*parsed);
                                    record.has_value())
                                {
                                    selected = record->name;
                                    if (const auto* type =
                                            world.Schemas().FindType(record->type);
                                        type != nullptr)
                                    {
                                        selected += " · ";
                                        selected += type->displayName;
                                    }
                                }
                                else
                                {
                                    selected = parsed->ToString();
                                }
                            }
                            context.KeyValue(parameterDisplayName, selected);

                            std::string chooseLabel =
                                "Choose…##quick-create-object-picker-" +
                                parameter.name;
                            const bool openPicker = context.Button(chooseLabel);
                            const std::string pickerSelectionKey =
                                "object:" + parameter.name;
                            if (openPicker)
                            {
                                quickCreatePickerQuery_[parameter.name].clear();
                                quickCreatePickerSelection_[pickerSelectionKey] = 0;
                            }
                            std::string popupId =
                                "quick-create-object-picker-popup-" +
                                parameter.name;
                            if (context.BeginPopup(
                                    popupId,
                                    openPicker,
                                    {.width = 440.0F * editor_ui::CurrentUiScale(),
                                     .height = 0.0F}))
                            {
                                context.Text("Choose Object");
                                if (openPicker)
                                {
                                    context.FocusNextItem();
                                }
                                auto& query =
                                    quickCreatePickerQuery_[parameter.name];
                                auto& resultSelection =
                                    quickCreatePickerSelection_[pickerSelectionKey];
                                if (context.InputText(
                                        "Search##quick-create-object-search",
                                        query))
                                {
                                    resultSelection = 0;
                                }
                                if (context.KeyPressed(editor_ui::UiKey::Escape))
                                {
                                    if (!query.empty())
                                    {
                                        query.clear();
                                        resultSelection = 0;
                                    }
                                    else
                                    {
                                        context.CloseCurrentPopup();
                                    }
                                }
                                context.Separator();

                                const auto selectedObjectId =
                                    scene::ObjectId::Parse(idText);
                                const auto chooseObject =
                                    [&](const scene::ObjectRecord& candidate)
                                    {
                                        found->second = candidate.id;
                                        idText = candidate.id.ToString();
                                        query.clear();
                                        context.CloseCurrentPopup();
                                    };

                                std::vector<scene::ObjectId> selectedObjectPath;
                                if (selectedObjectId.has_value())
                                {
                                    auto current =
                                        world.Objects().Find(*selectedObjectId);
                                    for (u32 depth = 0U;
                                         current.has_value() && depth < 64U;
                                         ++depth)
                                    {
                                        selectedObjectPath.push_back(current->id);
                                        if (!current->parent.has_value())
                                        {
                                            break;
                                        }
                                        current =
                                            world.Objects().Find(*current->parent);
                                    }
                                }

                                if (!query.empty())
                                {
                                    const auto candidates =
                                        world.Explorer().Search(query, 20U);
                                    if (candidates.empty())
                                    {
                                        resultSelection = 0;
                                        context.MutedText("No matching objects.");
                                    }
                                    else
                                    {
                                        const i32 resultCount =
                                            static_cast<i32>(candidates.size());
                                        resultSelection = std::clamp(
                                            resultSelection,
                                            0,
                                            resultCount - 1);
                                        if (context.KeyPressed(
                                                editor_ui::UiKey::Down,
                                                true))
                                        {
                                            resultSelection =
                                                (resultSelection + 1) % resultCount;
                                        }
                                        if (context.KeyPressed(
                                                editor_ui::UiKey::Up,
                                                true))
                                        {
                                            resultSelection =
                                                (resultSelection + resultCount - 1) %
                                                resultCount;
                                        }

                                        bool chosenByMouse = false;
                                        for (i32 index = 0;
                                             index < resultCount;
                                             ++index)
                                        {
                                            const auto& candidate =
                                                candidates[static_cast<std::size_t>(index)];
                                            std::string option = candidate.name;
                                            if (const auto* type =
                                                    world.Schemas().FindType(candidate.type);
                                                type != nullptr)
                                            {
                                                option += "\n";
                                                option += type->displayName;
                                            }
                                            option += " · ";
                                            option += candidate.id.ToString();
                                            option += "##object-picker-search-result-";
                                            option += candidate.id.ToString();

                                            if (context.Selectable(
                                                    option,
                                                    index == resultSelection))
                                            {
                                                chooseObject(candidate);
                                                chosenByMouse = true;
                                                break;
                                            }
                                        }
                                        if (!chosenByMouse &&
                                            context.KeyPressed(
                                                editor_ui::UiKey::Enter) &&
                                            resultSelection >= 0 &&
                                            resultSelection < resultCount)
                                        {
                                            chooseObject(
                                                candidates[static_cast<std::size_t>(
                                                    resultSelection)]);
                                        }
                                    }
                                }
                                else
                                {
                                    const auto selectedObjects =
                                        world.Selection().Ordered();
                                    if (!selectedObjects.empty())
                                    {
                                        context.MutedText("Selection");
                                        for (const auto selectedId : selectedObjects)
                                        {
                                            const auto candidate =
                                                world.Objects().Find(selectedId);
                                            if (!candidate.has_value())
                                            {
                                                continue;
                                            }

                                            std::string option = candidate->name;
                                            if (const auto* type =
                                                    world.Schemas().FindType(candidate->type);
                                                type != nullptr)
                                            {
                                                option += "\n";
                                                option += type->displayName;
                                            }
                                            option += "##object-picker-selected-";
                                            option += candidate->id.ToString();
                                            if (context.Selectable(
                                                    option,
                                                    selectedObjectId.has_value() &&
                                                        candidate->id == *selectedObjectId))
                                            {
                                                chooseObject(*candidate);
                                            }
                                        }
                                        context.Separator();
                                    }

                                    const auto roots = world.Explorer().Roots();
                                    if (roots.empty())
                                    {
                                        context.MutedText("No objects are available in this world.");
                                    }
                                    else
                                    {
                                        context.MutedText("Hierarchy");
                                        std::function<void(
                                            const scene::ObjectRecord&,
                                            u32)> drawObjectNode;
                                        drawObjectNode =
                                            [&](const scene::ObjectRecord& candidate,
                                                const u32 depth)
                                            {
                                                constexpr u32 kMaximumPickerDepth = 64U;
                                                const auto children =
                                                    depth < kMaximumPickerDepth
                                                        ? world.Explorer().Children(candidate.id)
                                                        : std::vector<scene::ObjectRecord>{};
                                                const bool isChosen =
                                                    selectedObjectId.has_value() &&
                                                    candidate.id == *selectedObjectId;
                                                const bool onSelectedPath =
                                                    std::ranges::find(
                                                        selectedObjectPath,
                                                        candidate.id) !=
                                                    selectedObjectPath.end();

                                                std::string typeName;
                                                if (const auto* type =
                                                        world.Schemas().FindType(candidate.type);
                                                    type != nullptr)
                                                {
                                                    typeName = type->displayName;
                                                }

                                                if (children.empty())
                                                {
                                                    std::string option = candidate.name;
                                                    if (!typeName.empty())
                                                    {
                                                        option += " · ";
                                                        option += typeName;
                                                    }
                                                    option += "##object-picker-leaf-";
                                                    option += candidate.id.ToString();
                                                    if (context.Selectable(option, isChosen))
                                                    {
                                                        chooseObject(candidate);
                                                    }
                                                    return;
                                                }

                                                std::string nodeLabel = candidate.name;
                                                if (!typeName.empty())
                                                {
                                                    nodeLabel += " · ";
                                                    nodeLabel += typeName;
                                                }
                                                nodeLabel += "##object-picker-node-";
                                                nodeLabel += candidate.id.ToString();
                                                context.SetNextTreeItemOpen(
                                                    onSelectedPath);
                                                const auto interaction =
                                                    context.TreeItem(nodeLabel, isChosen);
                                                context.SameLine();
                                                std::string chooseLabel =
                                                    "Choose##object-picker-choose-" +
                                                    candidate.id.ToString();
                                                if (context.Button(chooseLabel))
                                                {
                                                    chooseObject(candidate);
                                                }
                                                if (interaction.open)
                                                {
                                                    for (const auto& child : children)
                                                    {
                                                        drawObjectNode(child, depth + 1U);
                                                    }
                                                    context.TreePop();
                                                }
                                            };

                                        for (const auto& root : roots)
                                        {
                                            drawObjectNode(root, 0U);
                                        }
                                    }
                                }
                                context.EndPopup();
                            }

                            std::string advancedLabel =
                                "Advanced UUID##quick-create-object-uuid-" +
                                parameter.name;
                            if (context.Section(advancedLabel, false))
                            {
                                static_cast<void>(context.InputText(label, idText));
                            }
                            break;
                        }
                        case commands::CommandValueKind::PropertyId:
                        {
                            auto& idText = quickCreateIdText_[parameter.name];
                            std::string selected = "None";
                            if (const auto parsed = schema::PropertyId::Parse(idText);
                                parsed.has_value())
                            {
                                const auto catalog = world.Schemas().Catalog();
                                for (const auto& type : catalog)
                                {
                                    const auto property = std::ranges::find_if(
                                        type.properties,
                                        [&parsed](const schema::PropertySchema& item)
                                        {
                                            return item.id == *parsed;
                                        });
                                    if (property != type.properties.end())
                                    {
                                        selected = property->name +
                                            " · " + type.displayName;
                                        break;
                                    }
                                }
                                if (selected == "None")
                                {
                                    selected = parsed->ToString();
                                }
                            }
                            context.KeyValue(parameterDisplayName, selected);

                            std::string chooseLabel =
                                "Choose…##quick-create-property-picker-" +
                                parameter.name;
                            const bool openPicker = context.Button(chooseLabel);
                            const std::string pickerSelectionKey =
                                "property:" + parameter.name;
                            if (openPicker)
                            {
                                quickCreatePickerQuery_[parameter.name].clear();
                                quickCreatePickerSelection_[pickerSelectionKey] = 0;
                            }
                            std::string popupId =
                                "quick-create-property-picker-popup-" +
                                parameter.name;
                            if (context.BeginPopup(
                                    popupId,
                                    openPicker,
                                    {.width = 460.0F * editor_ui::CurrentUiScale(),
                                     .height = 0.0F}))
                            {
                                context.Text("Choose Property");
                                if (openPicker)
                                {
                                    context.FocusNextItem();
                                }
                                auto& query =
                                    quickCreatePickerQuery_[parameter.name];
                                auto& resultSelection =
                                    quickCreatePickerSelection_[pickerSelectionKey];
                                if (context.InputText(
                                        "Search##quick-create-property-search",
                                        query))
                                {
                                    resultSelection = 0;
                                }
                                if (context.KeyPressed(editor_ui::UiKey::Escape))
                                {
                                    if (!query.empty())
                                    {
                                        query.clear();
                                        resultSelection = 0;
                                    }
                                    else
                                    {
                                        context.CloseCurrentPopup();
                                    }
                                }
                                context.Separator();

                                std::optional<schema::TypeId> preferredType;
                                for (const auto& commandParameter :
                                     descriptor->parameters)
                                {
                                    if (commandParameter.kind !=
                                        commands::CommandValueKind::ObjectId)
                                    {
                                        continue;
                                    }
                                    const auto textIt =
                                        quickCreateIdText_.find(commandParameter.name);
                                    if (textIt == quickCreateIdText_.end())
                                    {
                                        continue;
                                    }
                                    const auto objectId =
                                        scene::ObjectId::Parse(textIt->second);
                                    if (!objectId.has_value())
                                    {
                                        continue;
                                    }
                                    if (const auto object =
                                            world.Objects().Find(*objectId);
                                        object.has_value())
                                    {
                                        preferredType = object->type;
                                        break;
                                    }
                                }
                                if (!preferredType.has_value() &&
                                    world.Selection().Ordered().size() == 1U)
                                {
                                    if (const auto selectedObject =
                                            world.Objects().Find(
                                                world.Selection().Ordered().front());
                                        selectedObject.has_value())
                                    {
                                        preferredType = selectedObject->type;
                                    }
                                }

                                const auto catalog = world.Schemas().Catalog();
                                const auto selectedPropertyId =
                                    schema::PropertyId::Parse(idText);
                                const auto chooseProperty =
                                    [&](const schema::PropertySchema& property)
                                    {
                                        found->second = property.id;
                                        idText = property.id.ToString();
                                        query.clear();
                                        context.CloseCurrentPopup();
                                    };

                                std::optional<schema::TypeId> selectedPropertyType;
                                if (selectedPropertyId.has_value())
                                {
                                    for (const auto& type : catalog)
                                    {
                                        const auto property = std::ranges::find_if(
                                            type.properties,
                                            [&selectedPropertyId](
                                                const schema::PropertySchema& item)
                                            {
                                                return item.id ==
                                                    *selectedPropertyId;
                                            });
                                        if (property != type.properties.end())
                                        {
                                            selectedPropertyType = type.id;
                                            break;
                                        }
                                    }
                                }

                                if (!query.empty())
                                {
                                    struct PropertyCandidate
                                    {
                                        const schema::TypeSchema* type{nullptr};
                                        const schema::PropertySchema* property{nullptr};
                                        bool preferred{false};
                                    };

                                    std::vector<PropertyCandidate> candidates;
                                    const std::string lowerQuery = PaletteLower(query);
                                    for (const auto& type : catalog)
                                    {
                                        for (const auto& property : type.properties)
                                        {
                                            std::string searchable =
                                                property.name + " " +
                                                type.displayName + " " +
                                                type.category;
                                            if (PaletteLower(searchable).find(lowerQuery) ==
                                                std::string::npos)
                                            {
                                                continue;
                                            }

                                            candidates.push_back({
                                                .type = &type,
                                                .property = &property,
                                                .preferred =
                                                    preferredType.has_value() &&
                                                    type.id == *preferredType
                                            });
                                        }
                                    }

                                    std::ranges::stable_sort(
                                        candidates,
                                        [](const PropertyCandidate& left,
                                           const PropertyCandidate& right)
                                        {
                                            if (left.preferred != right.preferred)
                                            {
                                                return left.preferred;
                                            }
                                            if (left.type->displayName !=
                                                right.type->displayName)
                                            {
                                                return left.type->displayName <
                                                    right.type->displayName;
                                            }
                                            return left.property->name <
                                                right.property->name;
                                        });
                                    if (candidates.size() > 24U)
                                    {
                                        candidates.resize(24U);
                                    }

                                    if (candidates.empty())
                                    {
                                        resultSelection = 0;
                                        context.MutedText(
                                            "No matching schema properties.");
                                    }
                                    else
                                    {
                                        const i32 resultCount =
                                            static_cast<i32>(candidates.size());
                                        resultSelection = std::clamp(
                                            resultSelection,
                                            0,
                                            resultCount - 1);
                                        if (context.KeyPressed(
                                                editor_ui::UiKey::Down,
                                                true))
                                        {
                                            resultSelection =
                                                (resultSelection + 1) % resultCount;
                                        }
                                        if (context.KeyPressed(
                                                editor_ui::UiKey::Up,
                                                true))
                                        {
                                            resultSelection =
                                                (resultSelection + resultCount - 1) %
                                                resultCount;
                                        }

                                        bool chosenByMouse = false;
                                        for (i32 index = 0;
                                             index < resultCount;
                                             ++index)
                                        {
                                            const auto& candidate =
                                                candidates[static_cast<std::size_t>(index)];
                                            std::string option =
                                                candidate.property->name;
                                            option += "\n";
                                            option += candidate.type->displayName;
                                            if (!candidate.property->unit.empty())
                                            {
                                                option += " · ";
                                                option += candidate.property->unit;
                                            }
                                            option += " · ";
                                            option += candidate.property->id.ToString();
                                            option += "##property-picker-search-result-";
                                            option += candidate.property->id.ToString();

                                            if (context.Selectable(
                                                    option,
                                                    index == resultSelection))
                                            {
                                                chooseProperty(*candidate.property);
                                                chosenByMouse = true;
                                                break;
                                            }
                                        }
                                        if (!chosenByMouse &&
                                            context.KeyPressed(
                                                editor_ui::UiKey::Enter) &&
                                            resultSelection >= 0 &&
                                            resultSelection < resultCount)
                                        {
                                            chooseProperty(
                                                *candidates[static_cast<std::size_t>(
                                                    resultSelection)].property);
                                        }
                                    }
                                }
                                else
                                {
                                    struct PropertyCategoryGroup
                                    {
                                        std::string name;
                                        std::vector<const schema::TypeSchema*> types;
                                        bool preferred{false};
                                    };

                                    std::vector<PropertyCategoryGroup> groups;
                                    for (const auto& type : catalog)
                                    {
                                        if (type.properties.empty())
                                        {
                                            continue;
                                        }

                                        const std::string category =
                                            type.category.empty()
                                                ? "Uncategorized"
                                                : type.category;
                                        auto group = std::ranges::find_if(
                                            groups,
                                            [&category](
                                                const PropertyCategoryGroup& item)
                                            {
                                                return item.name == category;
                                            });
                                        if (group == groups.end())
                                        {
                                            groups.push_back({
                                                .name = category
                                            });
                                            group = groups.end() - 1;
                                        }

                                        group->types.push_back(&type);
                                        if (preferredType.has_value() &&
                                            type.id == *preferredType)
                                        {
                                            group->preferred = true;
                                        }
                                    }

                                    std::ranges::stable_sort(
                                        groups,
                                        [](const PropertyCategoryGroup& left,
                                           const PropertyCategoryGroup& right)
                                        {
                                            if (left.preferred != right.preferred)
                                            {
                                                return left.preferred;
                                            }
                                            return left.name < right.name;
                                        });

                                    if (groups.empty())
                                    {
                                        context.MutedText(
                                            "No schema properties are registered.");
                                    }
                                    else
                                    {
                                        context.MutedText("Schema");
                                        for (std::size_t groupIndex = 0;
                                             groupIndex < groups.size();
                                             ++groupIndex)
                                        {
                                            auto& group = groups[groupIndex];
                                            std::ranges::stable_sort(
                                                group.types,
                                                [&preferredType](
                                                    const schema::TypeSchema* left,
                                                    const schema::TypeSchema* right)
                                                {
                                                    const bool leftPreferred =
                                                        preferredType.has_value() &&
                                                        left->id == *preferredType;
                                                    const bool rightPreferred =
                                                        preferredType.has_value() &&
                                                        right->id == *preferredType;
                                                    if (leftPreferred != rightPreferred)
                                                    {
                                                        return leftPreferred;
                                                    }
                                                    return left->displayName <
                                                        right->displayName;
                                                });

                                            std::string categoryLabel = group.name;
                                            categoryLabel += std::format(
                                                "##property-picker-category-{}",
                                                groupIndex);
                                            const bool categoryContainsSelected =
                                                selectedPropertyType.has_value() &&
                                                std::ranges::any_of(
                                                    group.types,
                                                    [&selectedPropertyType](
                                                        const schema::TypeSchema* type)
                                                    {
                                                        return type->id ==
                                                            *selectedPropertyType;
                                                    });
                                            context.SetNextTreeItemOpen(
                                                group.preferred ||
                                                categoryContainsSelected);
                                            const auto categoryInteraction =
                                                context.TreeItem(
                                                    categoryLabel,
                                                    false);
                                            if (!categoryInteraction.open)
                                            {
                                                continue;
                                            }

                                            for (const auto* type : group.types)
                                            {
                                                const bool typePreferred =
                                                    preferredType.has_value() &&
                                                    type->id == *preferredType;
                                                std::string typeLabel =
                                                    type->displayName;
                                                if (typePreferred)
                                                {
                                                    typeLabel += " · Current Type";
                                                }
                                                typeLabel +=
                                                    "##property-picker-type-";
                                                typeLabel += type->id.ToString();
                                                const bool typeContainsSelected =
                                                    selectedPropertyType.has_value() &&
                                                    type->id == *selectedPropertyType;
                                                context.SetNextTreeItemOpen(
                                                    typePreferred ||
                                                    typeContainsSelected);
                                                const auto typeInteraction =
                                                    context.TreeItem(
                                                        typeLabel,
                                                        false);
                                                if (!typeInteraction.open)
                                                {
                                                    continue;
                                                }

                                                for (const auto& property :
                                                     type->properties)
                                                {
                                                    std::string option =
                                                        property.name;
                                                    if (!property.unit.empty())
                                                    {
                                                        option += " · ";
                                                        option += property.unit;
                                                    }
                                                    option +=
                                                        "##property-picker-property-";
                                                    option += property.id.ToString();
                                                    if (context.Selectable(
                                                            option,
                                                            selectedPropertyId.has_value() &&
                                                                property.id ==
                                                                    *selectedPropertyId))
                                                    {
                                                        chooseProperty(property);
                                                    }
                                                }
                                                context.TreePop();
                                            }
                                            context.TreePop();
                                        }
                                    }
                                }
                                context.EndPopup();
                            }

                            std::string advancedLabel =
                                "Advanced UUID##quick-create-property-uuid-" +
                                parameter.name;
                            if (context.Section(advancedLabel, false))
                            {
                                static_cast<void>(context.InputText(label, idText));
                            }
                            break;
                        }
                        }
                        }

                        if (!parameter.description.empty())
                        {
                            context.MutedText(parameter.description);
                        }
                    }

                    if (!quickCreateArgumentError_.empty())
                    {
                        context.ErrorText(quickCreateArgumentError_);
                    }

                    if (context.PrimaryButton("Create##quick-create-argument-submit"))
                    {
                        commands::CommandArguments arguments;
                        std::string validationError;

                        for (const auto& parameter : descriptor->parameters)
                        {
                            const bool enabled = parameter.required ||
                                quickCreateArgumentEnabled_[parameter.name];
                            if (!enabled)
                            {
                                continue;
                            }

                            const auto found =
                                quickCreateArguments_.find(parameter.name);
                            if (found == quickCreateArguments_.end())
                            {
                                validationError =
                                    "Missing generated value for " +
                                    parameter.name + ".";
                                break;
                            }

                            if (parameter.kind ==
                                commands::CommandValueKind::ObjectId)
                            {
                                const auto parsed = scene::ObjectId::Parse(
                                    quickCreateIdText_[parameter.name]);
                                if (!parsed.has_value())
                                {
                                    validationError =
                                        parameter.name +
                                        " must be a valid object UUID.";
                                    break;
                                }
                                arguments[parameter.name] = *parsed;
                            }
                            else if (parameter.kind ==
                                     commands::CommandValueKind::PropertyId)
                            {
                                const auto parsed = schema::PropertyId::Parse(
                                    quickCreateIdText_[parameter.name]);
                                if (!parsed.has_value())
                                {
                                    validationError =
                                        parameter.name +
                                        " must be a valid property UUID.";
                                    break;
                                }
                                arguments[parameter.name] = *parsed;
                            }
                            else
                            {
                                arguments[parameter.name] = found->second;
                            }
                        }

                        if (!validationError.empty())
                        {
                            quickCreateArgumentError_ =
                                std::move(validationError);
                        }
                        else
                        {
                            try
                            {
                                registry.Invoke(
                                    quickCreateArgumentCommand_,
                                    arguments);
                                owner_->status_.clear();
                                clearArgumentForm();
                                quickCreateBrowseQuery_.clear();
                                quickCreateBrowseSelection_ = 0;
                                context.CloseCurrentPopup();
                            }
                            catch (const std::exception& exception)
                            {
                                quickCreateArgumentError_ =
                                    exception.what();
                                owner_->status_ = exception.what();
                            }
                        }
                    }

                    if (context.KeyPressed(editor_ui::UiKey::Escape))
                    {
                        clearArgumentForm();
                    }
                }
            }

            context.EndPopup();
            return;
        }

        std::vector<CommandPaletteEntry> creationEntries;
        creationEntries.reserve(commandCatalog.size());
        for (const auto& command : commandCatalog)
        {
            if (!IsQuickCreateCatalogEntry(command))
            {
                continue;
            }

            const auto paletteEntry =
                std::ranges::find_if(
                    palette,
                    [&command](const CommandPaletteEntry& entry)
                    {
                        return entry.command == command.id;
                    });
            if (paletteEntry == palette.end())
            {
                continue;
            }
            creationEntries.push_back(*paletteEntry);
        }

        const auto browsePluginCatalog =
            GlobalStudioUiContributions().Catalog(
                StudioContributionSurface::QuickCreate);
        for (const auto& contribution : browsePluginCatalog)
        {
            if (contribution.kind != StudioContributionKind::Command ||
                !contribution.command.IsValid())
            {
                continue;
            }

            const auto paletteEntry =
                std::ranges::find_if(
                    palette,
                    [&contribution](const CommandPaletteEntry& entry)
                    {
                        return entry.command == contribution.command;
                    });
            if (paletteEntry == palette.end() ||
                std::ranges::any_of(
                    creationEntries,
                    [&contribution](const CommandPaletteEntry& entry)
                    {
                        return entry.command == contribution.command;
                    }))
            {
                continue;
            }

            auto entry = *paletteEntry;
            if (!contribution.label.empty())
            {
                entry.label = contribution.label;
            }
            if (!contribution.category.empty())
            {
                entry.category = contribution.category;
            }
            creationEntries.push_back(std::move(entry));
        }

        const auto matches =
            SearchCommandPalette(
                creationEntries,
                quickCreateBrowseQuery_,
                creationEntries.size());

        std::vector<CommandPaletteEntry> visibleMatches;
        visibleMatches.reserve(12U);
        for (const auto& entry : matches)
        {
            if (!registry.Enablement(entry.command).enabled)
            {
                continue;
            }

            visibleMatches.push_back(entry);
            if (visibleMatches.size() >= 12U)
            {
                break;
            }
        }

        const auto invokeCreation =
            [&](const CommandPaletteEntry& entry)
            {
                if (entry.requiresArguments)
                {
                    beginArgumentForm(entry.command);
                    return false;
                }

                try
                {
                    registry.Invoke(entry.command);
                    owner_->status_.clear();
                    quickCreateBrowseQuery_.clear();
                    quickCreateBrowseSelection_ = 0;
                    context.CloseCurrentPopup();
                    return true;
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                    return false;
                }
            };

        if (context.KeyPressed(editor_ui::UiKey::Escape))
        {
            quickCreateBrowseQuery_.clear();
            quickCreateBrowseSelection_ = 0;
            context.CloseCurrentPopup();
        }
        else if (visibleMatches.empty())
        {
            quickCreateBrowseSelection_ = 0;
            context.MutedText(
                "No matching creation commands are enabled in the current context.");
        }
        else
        {
            const i32 visibleCount =
                static_cast<i32>(visibleMatches.size());
            quickCreateBrowseSelection_ = std::clamp(
                quickCreateBrowseSelection_,
                0,
                visibleCount - 1);

            if (context.KeyPressed(editor_ui::UiKey::Down, true))
            {
                quickCreateBrowseSelection_ =
                    (quickCreateBrowseSelection_ + 1) % visibleCount;
            }
            if (context.KeyPressed(editor_ui::UiKey::Up, true))
            {
                quickCreateBrowseSelection_ =
                    (quickCreateBrowseSelection_ + visibleCount - 1) %
                    visibleCount;
            }

            bool invoked = false;
            for (i32 index = 0; index < visibleCount; ++index)
            {
                const auto& entry =
                    visibleMatches[static_cast<std::size_t>(index)];
                std::string label = entry.label;
                const std::string secondary =
                    CommandPaletteSecondaryText(entry);
                if (!secondary.empty())
                {
                    label += "\n";
                    label += secondary;
                }
                label += "##quick-create-browser-";
                label += entry.command.ToString();

                if (context.Selectable(
                        label,
                        index == quickCreateBrowseSelection_))
                {
                    quickCreateBrowseSelection_ = index;
                    invoked = invokeCreation(entry);
                    if (invoked)
                    {
                        break;
                    }
                }
            }

            if (!invoked &&
                context.KeyPressed(editor_ui::UiKey::Enter))
            {
                static_cast<void>(
                    invokeCreation(
                        visibleMatches[static_cast<std::size_t>(
                            quickCreateBrowseSelection_)]));
            }
        }

        context.EndPopup();
    }

    static_cast<void>(DrawContributions(
        context,
        StudioContributionSurface::WorkspaceToolbar,
        true));
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

        if (target != nullptr)
        {
            static constexpr std::array<std::string_view, 4>
                kViewportModes{
                    "Perspective",
                    "Body Map",
                    "Debug",
                    "System"
                };
            static constexpr std::array<studio_session::ViewportMode, 4>
                kViewportModeValues{
                    studio_session::ViewportMode::Perspective,
                    studio_session::ViewportMode::BodyMap,
                    studio_session::ViewportMode::Debug,
                    studio_session::ViewportMode::System
                };

            i32 viewportMode = 0;
            switch (target->mode)
            {
            case studio_session::ViewportMode::Perspective:
                viewportMode = 0;
                break;
            case studio_session::ViewportMode::BodyMap:
                viewportMode = 1;
                break;
            case studio_session::ViewportMode::Debug:
                viewportMode = 2;
                break;
            case studio_session::ViewportMode::System:
                viewportMode = 3;
                break;
            }

            context.SameLine();
            if (context.Combo(
                    "##viewport-mode-compact",
                    kViewportModes,
                    viewportMode))
            {
                viewportMode = std::clamp(viewportMode, 0, 3);
                try
                {
                    owner_->InvokeViewportMode(
                        kViewportModeValues[
                            static_cast<std::size_t>(viewportMode)]);
                    owner_->status_.clear();
                    target = owner_->session_->Viewports().Find(id);
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }

            switch (target->mode)
            {
            case studio_session::ViewportMode::Perspective:
            {
                context.SameLine();
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
                break;
            }

            case studio_session::ViewportMode::Debug:
            {
                context.SameLine();
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
                break;
            }

            case studio_session::ViewportMode::BodyMap:
            case studio_session::ViewportMode::System:
                break;
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

    switch (viewportState_.gizmo.tool)
    {
    case GizmoTool::Select:
        break;

    case GizmoTool::Translate:
    {
        context.SameLine();
        static constexpr std::array<std::string_view, 4>
            kTranslationSnapModes{"Off", "Grid", "Surface", "Both"};
        i32 snapMode =
            (viewportState_.gizmo.translationSnap ? 1 : 0) |
            (viewportState_.gizmo.surfaceSnap ? 2 : 0);
        if (context.Combo(
                "##gizmo-translation-snap-mode",
                kTranslationSnapModes,
                snapMode))
        {
            viewportState_.gizmo.translationSnap =
                (snapMode & 1) != 0;
            viewportState_.gizmo.surfaceSnap =
                (snapMode & 2) != 0;
        }
        break;
    }

    case GizmoTool::Rotate:
    {
        context.SameLine();
        static constexpr std::array<std::string_view, 2>
            kRotationSnapModes{"Off", "Angle"};
        i32 snapMode = viewportState_.gizmo.rotationSnap ? 1 : 0;
        if (context.Combo(
                "##gizmo-rotation-snap-mode",
                kRotationSnapModes,
                snapMode))
        {
            viewportState_.gizmo.rotationSnap = snapMode != 0;
        }
        break;
    }

    case GizmoTool::Scale:
    {
        context.SameLine();
        static constexpr std::array<std::string_view, 2>
            kScaleSnapModes{"Off", "Step"};
        i32 snapMode = viewportState_.gizmo.scaleSnap ? 1 : 0;
        if (context.Combo(
                "##gizmo-scale-snap-mode",
                kScaleSnapModes,
                snapMode))
        {
            viewportState_.gizmo.scaleSnap = snapMode != 0;
        }
        break;
    }
    }

    static_cast<void>(DrawContributions(
        context,
        StudioContributionSurface::ContextToolbar,
        true));
}
} // namespace orbit::studio_ui
