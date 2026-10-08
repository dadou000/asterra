#include <orbit/studio_ui/CommandPaletteModel.hpp>
#include <orbit/studio_ui/InspectorProviderRegistry.hpp>
#include <orbit/studio_ui/StudioPersistentState.hpp>
#include <orbit/studio_ui/StudioShellModel.hpp>
#include <orbit/studio_ui/StudioUiContributions.hpp>
#include <orbit/studio_ui/ViewportAuthoringState.hpp>

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
using namespace orbit;
using namespace orbit::studio_ui;

void Check(const bool condition, const std::string_view what)
{
    if (!condition)
    {
        std::cerr << "Studio shell model test failed: " << what << "\n";
        std::exit(EXIT_FAILURE);
    }
}

void WorkspacesChooseTheExpectedBrowserSurface()
{
    Check(DefaultBrowserMode(StudioWorkspaceMode::Scene) == StudioBrowserMode::World, "Scene starts on World");
    Check(DefaultBrowserMode(StudioWorkspaceMode::Planet) == StudioBrowserMode::World, "Planet starts on World");
    Check(DefaultBrowserMode(StudioWorkspaceMode::Celestial) == StudioBrowserMode::World, "Celestial starts on World");
    Check(DefaultBrowserMode(StudioWorkspaceMode::Simulation) == StudioBrowserMode::World, "Simulation starts on World");
    Check(DefaultBrowserMode(StudioWorkspaceMode::Shading) == StudioBrowserMode::Assets, "Shading starts on Assets");

    Check(BrowserSourcePanelTitle(StudioBrowserMode::World) == std::string_view{"Explorer Source"}, "World maps to the internal Explorer source panel");
    Check(BrowserSourcePanelTitle(StudioBrowserMode::Assets) == std::string_view{"Material Service"}, "Assets map to Material Service");
    Check(StudioWorkspaceName(StudioWorkspaceMode::Scene) == "Build", "Build workspace label stable");
    Check(StudioWorkspaceName(StudioWorkspaceMode::Shading) == "Shading", "Shading name stable");
}

void WorkspacesShareOneCanonicalSpatialShell()
{
    Check(UsesCanonicalViewportWorkspace(StudioWorkspaceMode::Scene), "Scene uses canonical viewport shell");
    Check(UsesCanonicalViewportWorkspace(StudioWorkspaceMode::Planet), "Planet uses canonical viewport shell");
    Check(UsesCanonicalViewportWorkspace(StudioWorkspaceMode::Celestial), "Celestial uses canonical viewport shell");
    Check(UsesCanonicalViewportWorkspace(StudioWorkspaceMode::Simulation), "Simulation uses canonical viewport shell");
    Check(!UsesCanonicalViewportWorkspace(StudioWorkspaceMode::Shading), "Shading is the explicit specialist center surface");

    Check(WorkspaceCenterPanelTitle(StudioWorkspaceMode::Scene) == "Viewport", "Scene center is Viewport");
    Check(WorkspaceCenterPanelTitle(StudioWorkspaceMode::Celestial) == "Viewport", "Celestial keeps Viewport center");
    Check(WorkspaceCenterPanelTitle(StudioWorkspaceMode::Simulation) == "Viewport", "Simulation keeps Viewport center");
    Check(WorkspaceCenterPanelTitle(StudioWorkspaceMode::Shading) == "Shading", "Shading swaps only the center surface");
}

void InspectorProvidersAreOrderedAndOwnerScoped()
{
    InspectorProviderRegistry registry;
    bool surfaceRelevant = true;

    registry.Upsert({
        .id = "surface",
        .title = "Surface",
        .order = 20,
        .relevant = [&surfaceRelevant] { return surfaceRelevant; },
        .draw = [](editor_ui::PanelContext&) {}
    });
    registry.Upsert({
        .id = "properties",
        .title = "Properties",
        .order = 0,
        .defaultOpen = true,
        .draw = [](editor_ui::PanelContext&) {}
    });
    registry.Upsert({
        .id = "plugin.weather",
        .owner = "weather.plugin",
        .title = "Weather",
        .order = 10,
        .draw = [](editor_ui::PanelContext&) {}
    });

    auto relevant = registry.Relevant();
    Check(relevant.size() == 3U, "all relevant providers returned");
    Check(relevant[0]->id == "properties", "provider order begins with properties");
    Check(relevant[1]->id == "plugin.weather", "plugin provider order respected");
    Check(relevant[2]->id == "surface", "surface provider follows plugin");

    surfaceRelevant = false;
    Check(registry.Relevant().size() == 2U, "irrelevant providers are hidden");
    Check(registry.RemoveOwner("weather.plugin") == 1U, "plugin providers remove by owner");
    Check(registry.Relevant().size() == 1U, "owner removal leaves built-in provider");
}

void ContributionsAreStableAndOwnerScoped()
{
    StudioUiContributionRegistry registry;
    registry.Upsert({
        .id = "plugin.create",
        .owner = "plugin",
        .label = "Create Thing",
        .surface = StudioContributionSurface::QuickCreate,
        .kind = StudioContributionKind::Command,
        .order = 20,
        .command = {.high = 1, .low = 1}
    });
    registry.Upsert({
        .id = "orbit.create",
        .label = "Create Object",
        .surface = StudioContributionSurface::QuickCreate,
        .kind = StudioContributionKind::Command,
        .order = 0,
        .command = {.high = 1, .low = 2}
    });

    const auto catalog = registry.Catalog(StudioContributionSurface::QuickCreate);
    Check(catalog.size() == 2U, "quick-create contributions catalogued");
    Check(catalog[0].id == "orbit.create", "contribution order stable");
    Check(registry.RemoveOwner("plugin") == 1U, "plugin contributions remove by owner");
}

void CommandPaletteRanksUsefulMatches()
{
    const std::vector<commands::CommandCatalogEntry> catalog{
        {
            .id = {.high = 2, .low = 1},
            .name = "Add Point Light",
            .category = "Lighting",
            .description = "Creates a point light"
        },
        {
            .id = {.high = 2, .low = 2},
            .name = "Build Project",
            .category = "Build",
            .description = "Builds the active project"
        },
        {
            .id = {.high = 2, .low = 3},
            .name = "Connect Path Bezier",
            .category = "Path",
            .description = "Connect two path nodes",
            .parameters = {{.name = "mode", .kind = commands::CommandValueKind::String}}
        }
    };

    const auto entries = BuildCommandPalette(catalog);
    const auto light = SearchCommandPalette(entries, "point light");
    Check(!light.empty() && light.front().label == "Add Point Light", "palette ranks point light first");

    const auto build = SearchCommandPalette(entries, "Build");
    Check(!build.empty() && build.front().label == "Build Project", "palette prefix ranking works");

    const auto path = SearchCommandPalette(entries, "Bezier");
    Check(!path.empty() && path.front().requiresArguments, "palette exposes commands needing arguments");
}

void ViewportLayoutsClampAndRetainGizmoState()
{
    ViewportAuthoringState state;
    Check(state.SlotCount() == 1U, "single layout has one slot");

    state.SetLayout(ViewportLayout::Quad);
    state.SetActiveSlot(3U);
    Check(state.SlotCount() == 4U && state.activeSlot == 3U, "quad exposes four slots");

    state.SetLayout(ViewportLayout::VerticalSplit);
    Check(state.activeSlot == 1U, "layout reduction clamps active slot");

    state.gizmo.tool = GizmoTool::Translate;
    state.gizmo.space = GizmoSpace::Local;
    state.gizmo.surfaceSnap = true;
    Check(state.gizmo.tool == GizmoTool::Translate, "translate gizmo retained");
    Check(state.gizmo.space == GizmoSpace::Local, "local gizmo space retained");
    Check(state.gizmo.surfaceSnap, "surface snap retained");
}

void PersistentStateRoundTrips()
{
    StudioPersistentState state;
    state.workspace = StudioWorkspaceMode::Shading;
    state.browser = StudioBrowserMode::Assets;
    state.activityPanel = "Build";
    state.inspectorAdvanced = true;
    state.viewport.SetLayout(ViewportLayout::Quad);
    state.viewport.SetActiveSlot(3U);
    state.viewport.gizmo.tool = GizmoTool::Rotate;
    state.viewport.gizmo.rotationSnap = true;
    state.viewport.gizmo.rotationSnapDegrees = 22.5;
    state.viewport.gizmo.surfaceSnap = true;

    const auto parsed = ParseStudioPersistentState(
        SerializeStudioPersistentState(state));

    Check(parsed.workspace == StudioWorkspaceMode::Shading, "workspace persists");
    Check(parsed.browser == StudioBrowserMode::Assets, "browser tab persists");
    Check(parsed.activityPanel == "Build", "activity panel persists");
    Check(parsed.inspectorAdvanced, "Inspector advanced state persists");
    Check(parsed.viewport.layout == ViewportLayout::Quad, "viewport layout persists");
    Check(parsed.viewport.activeSlot == 3U, "active viewport persists");
    Check(parsed.viewport.gizmo.tool == GizmoTool::Rotate, "gizmo tool persists");
    Check(parsed.viewport.gizmo.rotationSnapDegrees == 22.5, "gizmo snap value persists");
    Check(parsed.viewport.gizmo.surfaceSnap, "surface snap persists");
}
} // namespace

int main()
{
    WorkspacesChooseTheExpectedBrowserSurface();
    WorkspacesShareOneCanonicalSpatialShell();
    InspectorProvidersAreOrderedAndOwnerScoped();
    ContributionsAreStableAndOwnerScoped();
    CommandPaletteRanksUsefulMatches();
    ViewportLayoutsClampAndRetainGizmoState();
    PersistentStateRoundTrips();
    return EXIT_SUCCESS;
}
