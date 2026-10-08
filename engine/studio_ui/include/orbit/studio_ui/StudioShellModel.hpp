#pragma once

#include <orbit/editor_ui/EditorUi.hpp>

#include <string_view>

namespace orbit::studio_ui
{
enum class StudioWorkspaceMode : u8
{
    Scene,
    Planet,
    Celestial,
    Simulation,
    Shading,
    Planning,
    Plugins
};

enum class StudioBrowserMode : u8
{
    World,
    Assets
};

struct WorldAssetsBrowserContract
{
    editor_ui::PanelId panel{};
    // Canonical Studio Explorer. The Explorer source panel supplies one
    // searchable world-and-assets tree; legacy browser mode is retained only
    // for persisted-state compatibility.
    bool defaultOpen{true};
    editor_ui::DockRegion defaultDock{editor_ui::DockRegion::Left};
    i32 dockOrder{-100};
    editor_ui::UiSize minSize{260.0F, 300.0F};
    editor_ui::UiSize defaultSize{340.0F, 820.0F};
};

inline constexpr WorldAssetsBrowserContract kWorldAssetsBrowserContract{
    .panel = {
        .high = 0x4f52424954535455ULL,
        .low = 0x574f524c44415354ULL
    }
};

[[nodiscard]] constexpr StudioBrowserMode DefaultBrowserMode(
    const StudioWorkspaceMode workspace) noexcept
{
    return workspace == StudioWorkspaceMode::Shading ||
           workspace == StudioWorkspaceMode::Plugins
        ? StudioBrowserMode::Assets
        : StudioBrowserMode::World;
}

[[nodiscard]] constexpr std::string_view BrowserSourcePanelTitle(
    const StudioBrowserMode mode) noexcept
{
    return mode == StudioBrowserMode::World
        ? std::string_view{"Explorer Source"}
        : std::string_view{"Material Service"};
}

// The common authoring modes deliberately share one spatial shell. Switching
// between Scene, Planet, Celestial and Simulation changes contextual tools,
// not the user's navigation model or panel geography.
[[nodiscard]] constexpr bool UsesCanonicalViewportWorkspace(
    const StudioWorkspaceMode workspace) noexcept
{
    return workspace == StudioWorkspaceMode::Scene ||
           workspace == StudioWorkspaceMode::Planet ||
           workspace == StudioWorkspaceMode::Celestial ||
           workspace == StudioWorkspaceMode::Simulation;
}

[[nodiscard]] constexpr std::string_view WorkspaceCenterPanelTitle(
    const StudioWorkspaceMode workspace) noexcept
{
    switch (workspace)
    {
    case StudioWorkspaceMode::Scene:
    case StudioWorkspaceMode::Planet:
    case StudioWorkspaceMode::Celestial:
    case StudioWorkspaceMode::Simulation:
        return "Viewport";
    case StudioWorkspaceMode::Shading: return "Shading";
    case StudioWorkspaceMode::Planning: return "Planning";
    case StudioWorkspaceMode::Plugins: return "Plugins";
    }
    return "Viewport";
}

[[nodiscard]] constexpr std::string_view StudioWorkspaceName(
    const StudioWorkspaceMode workspace) noexcept
{
    switch (workspace)
    {
    case StudioWorkspaceMode::Scene: return "Build";
    case StudioWorkspaceMode::Planet: return "Planet";
    case StudioWorkspaceMode::Celestial: return "Universe";
    case StudioWorkspaceMode::Simulation: return "Simulation";
    case StudioWorkspaceMode::Shading: return "Shading";
    case StudioWorkspaceMode::Planning: return "Planning";
    case StudioWorkspaceMode::Plugins: return "Plugins";
    }

    return "Scene";
}
} // namespace orbit::studio_ui
