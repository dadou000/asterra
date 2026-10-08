+++
path = "/editor/ui-toolkit"
title = "Editor UI toolkit (ImGui shell, panel extensions, previews)"
kind = "subsystem"
status = "stable"
summary = "The Dear ImGui based UI toolkit under Studio: the EditorUi shell (panels, trees, canvases, shell bands, key handling), panel extensions that let Studio and plugins add presentation to core panels, focus state, and the body and path preview renderers."
owner_module = "OrbitEditorUi"
keywords = ["editor ui", "imgui", "panel", "panel extension", "dock", "focus", "ui scale", "preview", "shell band", "toolkit"]
sources = [
  "engine/editor_ui/include/orbit/editor_ui/BodyPreviewRenderer.hpp",
  "engine/editor_ui/include/orbit/editor_ui/EditorUi.hpp",
  "engine/editor_ui/src/ShellBands.cpp",
  "engine/editor_ui/src/NavigationTabs.cpp",
  "engine/editor_ui/src/Toolbar.cpp",
  "engine/editor_ui/include/orbit/editor_ui/FocusState.hpp",
  "engine/editor_ui/include/orbit/editor_ui/PanelExtensions.hpp",
  "engine/editor_ui/include/orbit/editor_ui/PathPreviewRenderer.hpp",
  "engine/editor_ui/CMakeLists.txt",
]
symbols = ["PreviewMaterial", "UiSize", "PanelExtensionDefinition", "PreviewLine", "NavigationIcon", "NavigationTab", "ElementCategory", "ElementCategoryColor", "ToolbarIcon", "ToolbarChoice", "ToolbarStyle", "TreeItemWithIcon", "SelectableWithIcon"]
invariants = [
  "ToolbarButton and ToolbarChoices draw code-native vector icons with shared semantic category tints, transparent resting actions, blue selection accents, and disabled command states. NavigationTabs uses the same semantic palette for workspace icons while labels and selected indicators remain theme-colored. Category colors are presentation-only and never replace selection or status cues. ToolbarDivider separates inline groups; compact actions retain tooltips. ToolbarStyle::Modifier uses smaller type/icons and an inset fill; ToolbarStyle::Menu adds a dropdown chevron. State and execution remain with Studio command owners, and native saves use the central generation handoff.",
  "PanelContext::TreeItemWithIcon preserves native tree expansion and hit targets while drawing the visible label in a measured icon gutter; SelectableWithIcon uses the same spacing for selectable rows. Both accept opt-in vertical padding for panel-specific hit target sizing, use shared code-native toolbar icons and the same category tint mapping. InputText also accepts opt-in vertical padding.",
  "Panel extensions append presentation to an already-registered panel without taking ownership of that panel's docking, visibility or open/closed state; they are generic so Studio and hot-reloadable plugins can extend core panels.",
  "EditorUi's uiScale is 1.0 unless the window's DPI scale was above 1.0 at startup: ImGui's font size and style metrics already scale with it, so layout code must not rescale again.",
  "The focused-window title returned by FocusState aliases ImGui-owned storage and is valid only until that window is destroyed or renamed; copy it if it must live longer.",
  "The third-party ImGui library stays behind this module (/rules/architecture, rule 7).",
  "Emphasized shell bands use a neutral elevated surface with a lower shadow gutter. NavigationTabs draws consistent vector icons and labels, with the accent restricted to the active tab indicator; narrow layouts use icons with workspace tooltips.",
  "NavigationTabs is presentation only and returns the selected index to the existing workspace owner. Its native implementation follows the central Studio generation fallback and creates no GPU resource or reload boundary.",
]
related = ["/rules/ui", "/editor/viewport", "/authoring/plugins"]
depends_on = ["/foundation/core", "/foundation/frames", "/foundation/math", "/foundation/platform", "/rendering/render-view", "/rendering/rhi", "/rendering/shader-compiler", "/world/path-geometry", "/world/universe"]
used_by = ["/apps/studio", "/authoring/plugins", "/editor/studio-ui"]
verify = [
  "ctest -R Orbit.EditorDockLayout",
  "ctest -R Orbit.EditorPanelExtensions",
]
verified = "329654cd3290274cd945e9daec3fd214267ef2c8"
+++
