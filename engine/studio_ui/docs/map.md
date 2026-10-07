+++
path = "/editor/studio-ui"
title = "Studio UI map (panels, shell, continuity, captures, profiler)"
kind = "subsystem"
status = "stable"
summary = "A map of the large studio_ui module by responsibility: the app-level UiBundle and shell model, authoring panels (project, world documents, celestial, surface, volume, simulation, system view, shading, display diagnostics, reports, profiler), the viewport stack (panels, render-view set, navigation, camera, capture), view continuity, the flat planet map and the V0.0.7 validation/capture tooling."
owner_module = "OrbitStudioUi"
keywords = ["studio ui", "shell model", "view continuity", "viewport capture", "inspector extension", "flat map", "profiler panel", "panels", "workspace", "expansion shell", "authoring modes", "8k capture", "tiled capture"]
sources = [
  "engine/studio_ui/include/orbit/studio_ui/StudioShellModel.hpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioExpansionShell.hpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioInspectorExtension.hpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioViewContinuity.hpp",
  "engine/studio_ui/include/orbit/studio_ui/ViewportCaptureService.hpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioViewportCamera.hpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioViewportNavigation.hpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioFlatMap.hpp",
  "engine/studio_ui/include/orbit/studio_ui/ProfilerUi.hpp",
  "engine/studio_ui/include/orbit/studio_ui/ProfilerModel.hpp",
]
symbols = ["StudioExpansionShell", "StudioInspectorProviderRegistration", "StudioViewContinuity", "ViewportCaptureService", "ProfilerUi", "StudioTerrainNavigationConfig"]
invariants = [
  "Built-in authoring tools and hot-reloadable plugins share ONE inspector provider registry and ONE Properties-panel extension owned by StudioExpansionShell, so two visually identical contextual-inspector pipelines cannot drift; a registration lives exactly as long as its authoring UI instance, and headless/model workflows may populate the registry without an EditorUi host.",
  "There is one canonical Studio browser on the left (the world/assets browser contract); Explorer and Material Service stay registered as source implementations only. The authoring modes Scene, Planet, Celestial and Simulation share one spatial shell: switching changes contextual tools, never the navigation model or panel geography.",
  "Keyboard-first '+ Add' reuses the existing command palette (filtering for authoring verbs surfaces Create/Add/New descriptors with generated argument forms and project-asset pickers) instead of a second modal.",
  "View continuity (camera pose and simulation time of the primary view, saved to <project>/.orbit/StudioView.ini) is presentation state only and never world authority; unknown keys are ignored and a pose with a missing or non-finite field is dropped whole rather than half-applied.",
  "Viewport navigation controls are presentation-only: nothing in them participates in terrain authority, generation revisions or cache identity; the viewport camera converter takes a generation-stamped logical target, accepts a missing target (blank worlds) and REJECTS a stale universe target rather than rendering it against a replacement FrameGraph/BodyRegistry.",
  "High-resolution capture of the primary viewport: up to 8K the view is resized to the capture size and given a few frames to settle (temporal filtering, terrain streaming, lighting caches); larger shots (16K) do not fit in GPU memory as one render, so the camera is turned onto a grid of tiles with a narrower field of view and each tile is rendered at 4K.",
  "The flat planet map generates every layer from the same terrain samples (switching layers never re-samples the planet) and shares the HUD's convention: latitude = asin(direction.y) with +Y the spin pole, longitude = atan2(direction.z, direction.x), in degrees, on a 2:1 equirectangular image.",
  "Every profiler panel control has a profiler.panel_* RPC/MCP equivalent driving the same ProfilerModel (MCP parity).",
]
related = ["/editor/viewport", "/editor/mcp-rpc", "/editor/model", "/editor/session", "/editor/reports", "/rules/ui", "/legacy/orbit-profiler", "/rendering/shading"]
verify = [
  "ctest -R Orbit.StudioShellModel",
  "ctest -R Orbit.StudioInspectorExtension",
  "ctest -R Orbit.StudioViewContinuity",
  "ctest -R Orbit.ViewportCaptureTiling",
  "ctest -R Orbit.StudioFlatMap",
  "ctest -R Orbit.StudioViewportCamera",
  "ctest -R Orbit.StudioViewportNavigation",
  "ctest -R Orbit.ProfilerModel",
]
verified = "b0a0de7f"
[routes]
"Celestial panel, Celestial Tools, atmosphere preset buttons, System View canvas or orbit handles misbehave" = "/editor/studio-ui/celestial-authoring"
"Project Browser, World Documents, Project Settings, create/open project or world" = "/editor/studio-ui/world-and-project"
"Surface panel cache, revision or rebuild numbers look wrong, or a Surface edit has no RPC" = "/editor/studio-ui/surface-authoring"
"Volumes panel, Representation/LOD, cache bake, particle output, or Shading tab panel misbehaves" = "/editor/studio-ui/volume-authoring"
"Report has no screenshot, or reports.restore leaves the camera or clock wrong" = "/editor/studio-ui/reports"
"Text HUD, view.text_diagnostics, eye adaptation, Debug panel or transport band misbehaves; a Display Diagnostics control has no RPC" = "/editor/studio-ui/diagnostics-hud"
"V0.0.7 validation commands, performance JSON, 16K captures, or what is still unvalidated" = "/editor/studio-ui/validation-and-capture"
+++

Area to file map (`engine/studio_ui/include/orbit/studio_ui`): shell and persistence - `StudioShellModel`, `StudioExpansionShell`, `StudioPersistentState`, `StudioViewContinuity`, `StudioUiContributions`; authoring panels - `ProjectAuthoringUi`, `ProjectSettingsUi`, `WorldDocumentsUi`, `CelestialAuthoringUi`, `SurfaceAuthoringUi`, `VolumeAuthoringUi`, `SimulationControlsUi`, `SystemViewUi`, `ShadingUi`; viewport - see `/editor/viewport`; diagnostics - `DisplayDiagnosticsUi`, `DisplayEyeRpc`, `StudioTextDiagnosticsHud`, `ProfilerUi`/`ProfilerModel`/`ProfilerPanelRpc`, `ReportsUi`; release evidence - `V007ValidationScenarios`, `V007ValidationCommands`.
