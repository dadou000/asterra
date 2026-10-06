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
  "engine/editor_ui/include/orbit/editor_ui/FocusState.hpp",
  "engine/editor_ui/include/orbit/editor_ui/PanelExtensions.hpp",
  "engine/editor_ui/include/orbit/editor_ui/PathPreviewRenderer.hpp",
  "engine/editor_ui/CMakeLists.txt",
]
symbols = ["PreviewMaterial", "UiSize", "PanelExtensionDefinition", "PreviewLine"]
invariants = [
  "Panel extensions append presentation to an already-registered panel without taking ownership of that panel's docking, visibility or open/closed state; they are generic so Studio and hot-reloadable plugins can extend core panels.",
  "EditorUi's uiScale is 1.0 unless the window's DPI scale was above 1.0 at startup: ImGui's font size and style metrics already scale with it, so layout code must not rescale again.",
  "The focused-window title returned by FocusState aliases ImGui-owned storage and is valid only until that window is destroyed or renamed; copy it if it must live longer.",
  "The third-party ImGui library stays behind this module (/rules/architecture, rule 7).",
]
related = ["/rules/ui", "/editor/viewport", "/authoring/plugins"]
depends_on = ["/foundation/core", "/foundation/frames", "/foundation/math", "/foundation/platform", "/rendering/render-view", "/rendering/rhi", "/rendering/shader-compiler", "/world/path-geometry", "/world/universe"]
used_by = ["/apps/studio", "/authoring/plugins"]
verify = [
  "ctest -R Orbit.EditorDockLayout",
  "ctest -R Orbit.EditorPanelExtensions",
]
verified = "b0a0de7f"
+++


