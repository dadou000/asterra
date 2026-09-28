#include <orbit/studio_ui/CommandPaletteModel.hpp>
#include <orbit/studio_ui/InspectorProviderRegistry.hpp>
#include <orbit/studio_ui/StudioPersistentState.hpp>
#include <orbit/studio_ui/StudioUiContributions.hpp>
#include <orbit/studio_ui/ViewportAuthoringState.hpp>
#include <orbit/studio_ui/WorldAssetsBrowserModel.hpp>

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
using namespace orbit;
using namespace orbit::studio_ui;

void Check(const bool condition, const std::string_view message)
{
    if (!condition)
    {
        std::cerr << "Studio editor expansion test failed: "
                  << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void InspectorProvidersAreOrderedAndReplaceable()
{
    InspectorProviderRegistry registry;
    bool surfaceRelevant = true;

    registry.Upsert({
        .id = "surface",
        .owner = "orbit",
        .title = "Surface",
        .order = 20,
        .relevant = [&surfaceRelevant] { return surfaceRelevant; },
        .draw = [](editor_ui::PanelContext&) {}
    });
    registry.Upsert({
        .id = "properties",
        .owner = "orbit",
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
    Check(relevant[0]->id == "properties", "properties ordered first");
    Check(relevant[1]->id == "plugin.weather", "plugin order respected");
    Check(relevant[2]->id == "surface", "surface ordered last");

    surfaceRelevant = false;
    relevant = registry.Relevant();
    Check(relevant.size() == 2U, "irrelevant provider hidden");
    Check(registry.RemoveOwner("weather.plugin") == 1U, "owner removal works");
    Check(registry.Relevant().size() == 1U, "plugin provider removed cleanly");
}

void ContributionsAreStableAndOwnerScoped()
{
    StudioUiContributionRegistry registry;
    const commands::CommandId one{.high = 1, .low = 1};
    const commands::CommandId two{.high = 1, .low = 2};

    registry.Upsert({
        .id = "plugin.create",
        .owner = "plugin",
        .label = "Create Thing",
        .surface = StudioContributionSurface::QuickCreate,
        .kind = StudioContributionKind::Command,
        .order = 20,
        .command = one
    });
    registry.Upsert({
        .id = "orbit.create",
        .label = "Create Object",
        .surface = StudioContributionSurface::QuickCreate,
        .kind = StudioContributionKind::Command,
        .order = 0,
        .command = two
    });

    const auto catalog = registry.Catalog(
        StudioContributionSurface::QuickCreate);
    Check(catalog.size() == 2U, "quick-create contributions catalogued");
    Check(catalog[0].id == "orbit.create", "contribution order stable");
    Check(registry.RemoveOwner("plugin") == 1U, "plugin contributions removable");
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
    Check(!light.empty(), "palette finds point light");
    Check(light.front().label == "Add Point Light", "best command ranked first");

    const auto build = SearchCommandPalette(entries, "Build");
    Check(!build.empty() && build.front().label == "Build Project", "prefix ranking works");

    const auto path = SearchCommandPalette(entries, "Bezier");
    Check(!path.empty() && path.front().requiresArguments, "command arguments surfaced");
}

void ViewportLayoutsClampAndExposeGizmoState()
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
    Check(state.gizmo.surfaceSnap, "surface snap retained");
}

content::AssetRecord Asset(
    const u64 low,
    const content::AssetKind kind,
    std::string name,
    std::string path,
    std::vector<std::string> tags = {})
{
    return {
        .id = {.high = 0xA55E7, .low = low},
        .kind = kind,
        .name = std::move(name),
        .sourcePath = std::move(path),
        .tags = std::move(tags)
    };
}

void AssetBrowserSupportsCollectionsAndSearch()
{
    const auto vehicle = Asset(
        1,
        content::AssetKind::Component,
        "Harlow Coupe",
        "Content/Vehicles/HarlowCoupe.component",
        {"vehicle", "blueprint", "v8"});
    const auto material = Asset(
        2,
        content::AssetKind::Material,
        "Paint Blue",
        "Content/Materials/PaintBlue.material",
        {"paint"});

    WorldAssetsBrowserModel browser;
    browser.SetCategory(AssetBrowserCategory::Vehicles);
    Check(browser.Matches(vehicle), "vehicle category uses asset tags");
    Check(!browser.Matches(material), "vehicle category rejects material");

    browser.SetCategory(AssetBrowserCategory::All);
    browser.SetQuery("harlow");
    Check(browser.Matches(vehicle), "query searches asset names");

    browser.SetQuery({});
    browser.SetSearchChips({"v8"});
    Check(browser.Matches(vehicle), "search chips search tags");

    browser.SetSearchChips({});
    browser.ToggleFavorite(vehicle.id);
    browser.SetCategory(AssetBrowserCategory::Favorites);
    Check(browser.Matches(vehicle), "favorites collection works");

    browser.RecordRecent(material.id);
    browser.RecordRecent(vehicle.id);
    browser.SetCategory(AssetBrowserCategory::Recent);
    Check(browser.Matches(vehicle) && browser.Matches(material), "recent collection works");
    Check(browser.Recent().front() == vehicle.id, "most recent asset is first");
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
    Check(parsed.viewport.gizmo.rotationSnapDegrees == 22.5, "snap value persists");
    Check(parsed.viewport.gizmo.surfaceSnap, "surface snap persists");
}
} // namespace

int main()
{
    InspectorProvidersAreOrderedAndReplaceable();
    ContributionsAreStableAndOwnerScoped();
    CommandPaletteRanksUsefulMatches();
    ViewportLayoutsClampAndExposeGizmoState();
    AssetBrowserSupportsCollectionsAndSearch();
    PersistentStateRoundTrips();
    return EXIT_SUCCESS;
}
