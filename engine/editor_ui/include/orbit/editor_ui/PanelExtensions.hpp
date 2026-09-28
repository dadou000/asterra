#pragma once

#include <orbit/editor_ui/EditorUi.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace orbit::editor_ui
{
// Appends presentation to an already-registered EditorUi panel without taking
// ownership of that panel's docking, visibility or open/closed state. This is
// intentionally generic so Studio and hot-reloadable plugins can extend core
// panels without depending on their implementation translation units.
struct PanelExtensionDefinition
{
    std::string id;
    std::string targetTitle;
    i32 order{100};
    std::function<void(PanelContext&)> draw;
};

// Upsert is scoped to the currently active EditorUi/ImGui context, mirroring
// shell-band lifetime semantics. Stable IDs make plugin reload idempotent.
void UpsertPanelExtension(PanelExtensionDefinition extension);

[[nodiscard]] bool RemovePanelExtension(
    std::string_view id) noexcept;
} // namespace orbit::editor_ui
