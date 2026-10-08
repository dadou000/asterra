#pragma once

#include <orbit/studio_ui/StudioShellModel.hpp>
#include <orbit/studio_ui/ViewportAuthoringState.hpp>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace orbit::studio_ui
{
struct StudioPersistentState
{
    u32 version{1};
    StudioWorkspaceMode workspace{StudioWorkspaceMode::Scene};
    StudioBrowserMode browser{StudioBrowserMode::World};
    std::string activityPanel;
    bool inspectorAdvanced{false};
    ViewportAuthoringState viewport{};
};

[[nodiscard]] constexpr std::string_view ViewportLayoutName(
    const ViewportLayout layout) noexcept
{
    switch (layout)
    {
    case ViewportLayout::Single: return "single";
    case ViewportLayout::VerticalSplit: return "vertical";
    case ViewportLayout::HorizontalSplit: return "horizontal";
    case ViewportLayout::Quad: return "quad";
    }
    return "single";
}

[[nodiscard]] constexpr std::string_view ViewportModeName(
    const studio_session::ViewportMode mode) noexcept
{
    switch (mode)
    {
    case studio_session::ViewportMode::Perspective: return "perspective";
    case studio_session::ViewportMode::BodyMap: return "body_map";
    case studio_session::ViewportMode::Debug: return "debug";
    case studio_session::ViewportMode::System: return "system";
    case studio_session::ViewportMode::FlatMap: return "flat_map";
    }
    return "perspective";
}

[[nodiscard]] inline std::optional<StudioWorkspaceMode>
ParseWorkspaceMode(const std::string_view value) noexcept
{
    if (value == "Build" || value == "Scene") return StudioWorkspaceMode::Scene;
    if (value == "Planet") return StudioWorkspaceMode::Planet;
    if (value == "Universe" || value == "Celestial") return StudioWorkspaceMode::Celestial;
    if (value == "Simulation") return StudioWorkspaceMode::Simulation;
    if (value == "Shading") return StudioWorkspaceMode::Shading;
    if (value == "Planning") return StudioWorkspaceMode::Planning;
    if (value == "Plugins") return StudioWorkspaceMode::Plugins;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<ViewportLayout>
ParseViewportLayout(const std::string_view value) noexcept
{
    if (value == "single") return ViewportLayout::Single;
    if (value == "vertical") return ViewportLayout::VerticalSplit;
    if (value == "horizontal") return ViewportLayout::HorizontalSplit;
    if (value == "quad") return ViewportLayout::Quad;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<studio_session::ViewportMode>
ParseViewportMode(const std::string_view value) noexcept
{
    using studio_session::ViewportMode;
    if (value == "perspective") return ViewportMode::Perspective;
    if (value == "body_map") return ViewportMode::BodyMap;
    if (value == "debug") return ViewportMode::Debug;
    if (value == "system") return ViewportMode::System;
    if (value == "flat_map") return ViewportMode::FlatMap;
    return std::nullopt;
}

[[nodiscard]] inline std::string SerializeStudioPersistentState(
    const StudioPersistentState& state)
{
    std::ostringstream output;
    output << "version=" << state.version << '\n';
    output << "workspace=" << StudioWorkspaceName(state.workspace) << '\n';
    output << "browser="
           << (state.browser == StudioBrowserMode::Assets ? "assets" : "world")
           << '\n';
    output << "activity=" << state.activityPanel << '\n';
    output << "inspector_advanced="
           << (state.inspectorAdvanced ? "1" : "0") << '\n';
    output << "viewport_layout=" << ViewportLayoutName(state.viewport.layout)
           << '\n';
    output << "viewport_active=" << static_cast<u32>(state.viewport.activeSlot)
           << '\n';

    for (std::size_t index = 0; index < state.viewport.modes.size(); ++index)
    {
        output << "viewport_mode_" << index << '='
               << ViewportModeName(state.viewport.modes[index]) << '\n';
    }

    output << "gizmo_tool=" << static_cast<u32>(state.viewport.gizmo.tool) << '\n';
    output << "gizmo_space=" << static_cast<u32>(state.viewport.gizmo.space) << '\n';
    output << "gizmo_pivot=" << static_cast<u32>(state.viewport.gizmo.pivot) << '\n';
    output << "translation_snap="
           << (state.viewport.gizmo.translationSnap ? "1" : "0") << '\n';
    output << "translation_snap_m="
           << state.viewport.gizmo.translationSnapMeters << '\n';
    output << "translation_snap_unit=" << static_cast<u32>(state.viewport.gizmo.translationSnapUnit) << '\n';
    output << "rotation_snap="
           << (state.viewport.gizmo.rotationSnap ? "1" : "0") << '\n';
    output << "rotation_snap_deg="
           << state.viewport.gizmo.rotationSnapDegrees << '\n';
    output << "scale_snap="
           << (state.viewport.gizmo.scaleSnap ? "1" : "0") << '\n';
    output << "scale_snap_step=" << state.viewport.gizmo.scaleSnapStep << '\n';
    output << "surface_snap="
           << (state.viewport.gizmo.surfaceSnap ? "1" : "0") << '\n';
    return output.str();
}

[[nodiscard]] inline StudioPersistentState ParseStudioPersistentState(
    const std::string_view text)
{
    StudioPersistentState state;
    std::istringstream input(std::string{text});
    std::string line;

    const auto parseUnsigned = [](const std::string_view value)
        -> std::optional<u32>
    {
        u32 result = 0;
        const auto parsed = std::from_chars(
            value.data(), value.data() + value.size(), result);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != value.data() + value.size())
        {
            return std::nullopt;
        }
        return result;
    };

    const auto parseDouble = [](const std::string_view value)
        -> std::optional<f64>
    {
        try
        {
            std::size_t consumed = 0;
            const f64 result = std::stod(std::string{value}, &consumed);
            return consumed == value.size()
                ? std::optional<f64>{result}
                : std::nullopt;
        }
        catch (...)
        {
            return std::nullopt;
        }
    };

    while (std::getline(input, line))
    {
        const auto split = line.find('=');
        if (split == std::string::npos)
        {
            continue;
        }

        const std::string_view key{line.data(), split};
        const std::string_view value{
            line.data() + split + 1U,
            line.size() - split - 1U};

        if (key == "version")
        {
            if (const auto parsed = parseUnsigned(value)) state.version = *parsed;
        }
        else if (key == "workspace")
        {
            if (const auto parsed = ParseWorkspaceMode(value)) state.workspace = *parsed;
        }
        else if (key == "browser")
        {
            state.browser = value == "assets"
                ? StudioBrowserMode::Assets
                : StudioBrowserMode::World;
        }
        else if (key == "activity") state.activityPanel = value;
        else if (key == "inspector_advanced") state.inspectorAdvanced = value == "1";
        else if (key == "viewport_layout")
        {
            if (const auto parsed = ParseViewportLayout(value)) state.viewport.SetLayout(*parsed);
        }
        else if (key == "viewport_active")
        {
            if (const auto parsed = parseUnsigned(value))
                state.viewport.SetActiveSlot(static_cast<u8>(*parsed));
        }
        else if (key.starts_with("viewport_mode_"))
        {
            const auto indexText = key.substr(std::string_view{"viewport_mode_"}.size());
            const auto index = parseUnsigned(indexText);
            const auto mode = ParseViewportMode(value);
            if (index.has_value() && mode.has_value() &&
                *index < state.viewport.modes.size())
            {
                state.viewport.modes[*index] = *mode;
            }
        }
        else if (key == "gizmo_tool")
        {
            if (const auto parsed = parseUnsigned(value); parsed && *parsed <= 3U)
                state.viewport.gizmo.tool = static_cast<GizmoTool>(*parsed);
        }
        else if (key == "gizmo_space")
        {
            if (const auto parsed = parseUnsigned(value); parsed && *parsed <= 1U)
                state.viewport.gizmo.space = static_cast<GizmoSpace>(*parsed);
        }
        else if (key == "gizmo_pivot")
        {
            if (const auto parsed = parseUnsigned(value); parsed && *parsed <= 2U)
                state.viewport.gizmo.pivot = static_cast<GizmoPivot>(*parsed);
        }
        else if (key == "translation_snap") state.viewport.gizmo.translationSnap = value == "1";
        else if (key == "translation_snap_m")
        {
            if (const auto parsed = parseDouble(value)) state.viewport.gizmo.translationSnapMeters = *parsed;
        }
        else if (key == "translation_snap_unit")
        {
            if (const auto parsed = parseUnsigned(value); parsed && *parsed <= 5U)
                state.viewport.gizmo.translationSnapUnit = static_cast<SnapLengthUnit>(*parsed);
        }
        else if (key == "rotation_snap") state.viewport.gizmo.rotationSnap = value == "1";
        else if (key == "rotation_snap_deg")
        {
            if (const auto parsed = parseDouble(value)) state.viewport.gizmo.rotationSnapDegrees = *parsed;
        }
        else if (key == "scale_snap") state.viewport.gizmo.scaleSnap = value == "1";
        else if (key == "scale_snap_step")
        {
            if (const auto parsed = parseDouble(value)) state.viewport.gizmo.scaleSnapStep = *parsed;
        }
        else if (key == "surface_snap") state.viewport.gizmo.surfaceSnap = value == "1";
    }

    return state;
}

[[nodiscard]] inline StudioPersistentState LoadStudioPersistentState(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    return ParseStudioPersistentState(contents.str());
}

inline void SaveStudioPersistentState(
    const std::filesystem::path& path,
    const StudioPersistentState& state)
{
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";

    {
        std::ofstream output(
            temporary,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Failed to open Studio state file for writing.");
        }
        output << SerializeStudioPersistentState(state);
        if (!output)
        {
            throw std::runtime_error(
                "Failed to write Studio state file.");
        }
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        std::filesystem::remove(temporary, error);
        throw std::runtime_error(
            "Failed to replace Studio state file.");
    }
}
} // namespace orbit::studio_ui
