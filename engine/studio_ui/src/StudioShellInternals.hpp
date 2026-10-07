#pragma once

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

namespace orbit::studio_ui::shell_detail
{
extern InspectorProviderRegistry g_inspectorProviders;
extern StudioUiContributionRegistry g_uiContributions;

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

[[nodiscard]] inline bool IsQuickCreateLabel(
    const std::string_view label) noexcept
{
    return label.starts_with("Add ") ||
        label.starts_with("Create ") ||
        label.starts_with("New ");
}

[[nodiscard]] inline bool IsQuickCreateCatalogEntry(
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

[[nodiscard]] inline std::string CommandPaletteSecondaryText(
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

[[nodiscard]] inline bool IsTerrainSplineTool(
    const StudioTerrainAuthoringTool tool) noexcept
{
    return tool == StudioTerrainAuthoringTool::Canyon ||
        tool == StudioTerrainAuthoringTool::Ridge;
}

[[nodiscard]] inline const char* ViewportModeLabel(
    const studio_session::ViewportMode mode) noexcept
{
    switch (mode)
    {
    case studio_session::ViewportMode::Perspective: return "Perspective";
    case studio_session::ViewportMode::BodyMap: return "Body Map";
    case studio_session::ViewportMode::Debug: return "Debug";
    case studio_session::ViewportMode::System: return "System";
    case studio_session::ViewportMode::FlatMap: return "Flat Map";
    }
    return "Perspective";
}

[[nodiscard]] inline const char* EdgeName(
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

[[nodiscard]] inline const char* CubeFaceName(
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

} // namespace orbit::studio_ui::shell_detail
