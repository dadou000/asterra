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
    Shading
};

enum class StudioBrowserMode : u8
{
    World,
    Assets
};

struct WorldAssetsBrowserContract
{
    editor_ui::PanelId panel{};
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
    return workspace == StudioWorkspaceMode::Shading
        ? StudioBrowserMode::Assets
        : StudioBrowserMode::World;
}

[[nodiscard]] constexpr std::string_view BrowserSourcePanelTitle(
    const StudioBrowserMode mode) noexcept
{
    return mode == StudioBrowserMode::World
        ? std::string_view{"Explorer"}
        : std::string_view{"Material Service"};
}

[[nodiscard]] constexpr std::string_view StudioWorkspaceName(
    const StudioWorkspaceMode workspace) noexcept
{
    switch (workspace)
    {
    case StudioWorkspaceMode::Scene: return "Scene";
    case StudioWorkspaceMode::Planet: return "Planet";
    case StudioWorkspaceMode::Celestial: return "Celestial";
    case StudioWorkspaceMode::Simulation: return "Simulation";
    case StudioWorkspaceMode::Shading: return "Shading";
    }

    return "Scene";
}
} // namespace orbit::studio_ui
