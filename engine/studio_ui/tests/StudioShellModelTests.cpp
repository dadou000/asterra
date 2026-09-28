#include <orbit/studio_ui/StudioShellModel.hpp>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
using orbit::studio_ui::BrowserSourcePanelTitle;
using orbit::studio_ui::DefaultBrowserMode;
using orbit::studio_ui::StudioBrowserMode;
using orbit::studio_ui::StudioWorkspaceMode;
using orbit::studio_ui::StudioWorkspaceName;
using orbit::studio_ui::kWorldAssetsBrowserContract;

void Check(const bool condition, const char* what)
{
    if (!condition)
    {
        std::cerr << "Studio shell model test failed: " << what << "\n";
        std::exit(EXIT_FAILURE);
    }
}

void WorldAssetsPanelOwnsTheCompactLeftFrontDoor()
{
    const auto& contract =
        kWorldAssetsBrowserContract;

    Check(contract.panel.IsValid(), "browser panel id is stable and valid");
    Check(contract.defaultOpen, "browser opens by default");
    Check(
        contract.defaultDock == orbit::editor_ui::DockRegion::Left,
        "browser belongs to the left dock");
    Check(contract.dockOrder < 0, "browser is the first left-dock tab");
    Check(contract.minSize.width >= 240.0F, "browser keeps a usable minimum width");
    Check(contract.defaultSize.width <= 360.0F, "browser stays compact by default");
}

void WorkspacesChooseTheExpectedBrowserSurface()
{
    Check(
        DefaultBrowserMode(StudioWorkspaceMode::Scene) ==
            StudioBrowserMode::World,
        "Scene starts on World");
    Check(
        DefaultBrowserMode(StudioWorkspaceMode::Planet) ==
            StudioBrowserMode::World,
        "Planet starts on World");
    Check(
        DefaultBrowserMode(StudioWorkspaceMode::Celestial) ==
            StudioBrowserMode::World,
        "Celestial starts on World");
    Check(
        DefaultBrowserMode(StudioWorkspaceMode::Simulation) ==
            StudioBrowserMode::World,
        "Simulation starts on World");
    Check(
        DefaultBrowserMode(StudioWorkspaceMode::Shading) ==
            StudioBrowserMode::Assets,
        "Shading starts on Assets");
}

void BrowserModesComposeTheAuthoritativeLegacySurfaces()
{
    Check(
        BrowserSourcePanelTitle(StudioBrowserMode::World) ==
            std::string_view{"Explorer"},
        "World composes Explorer");
    Check(
        BrowserSourcePanelTitle(StudioBrowserMode::Assets) ==
            std::string_view{"Material Service"},
        "Assets composes Material Service");
}

void WorkspaceNamesRemainStableForShellAndAutomation()
{
    Check(StudioWorkspaceName(StudioWorkspaceMode::Scene) == "Scene", "Scene name");
    Check(StudioWorkspaceName(StudioWorkspaceMode::Planet) == "Planet", "Planet name");
    Check(StudioWorkspaceName(StudioWorkspaceMode::Celestial) == "Celestial", "Celestial name");
    Check(StudioWorkspaceName(StudioWorkspaceMode::Simulation) == "Simulation", "Simulation name");
    Check(StudioWorkspaceName(StudioWorkspaceMode::Shading) == "Shading", "Shading name");
}
} // namespace

int main()
{
    WorldAssetsPanelOwnsTheCompactLeftFrontDoor();
    WorkspacesChooseTheExpectedBrowserSurface();
    BrowserModesComposeTheAuthoritativeLegacySurfaces();
    WorkspaceNamesRemainStableForShellAndAutomation();
    return EXIT_SUCCESS;
}
