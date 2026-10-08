+++
path = "/editor/studio-ui"
title = "Studio UI map (panels, shell, continuity, captures, profiler)"
kind = "subsystem"
status = "stable"
summary = "A map of the large studio_ui module by responsibility: the app-level UiBundle and shell model, authoring panels (project, world documents, celestial, surface, volume, simulation, system view, shading, display diagnostics, reports, profiler), the viewport stack (panels, render-view set, navigation, camera, capture), view continuity, the flat planet map and the optional scene acceptance tooling and viewport capture."
owner_module = "OrbitStudioUi"
keywords = ["studio ui", "shell model", "view continuity", "viewport capture", "inspector extension", "flat map", "profiler panel", "panels", "workspace", "expansion shell", "authoring modes", "8k capture", "tiled capture"]
sources = [
  "engine/studio_ui/src/StudioShellQuickCreatePopup.cpp",
  "engine/studio_ui/src/StudioShellQuickCreateBrowser.cpp",
  "engine/studio_ui/src/StudioShellCommandPalettePopup.cpp",
  "engine/studio_ui/src/StudioShellViewportBand.cpp",
  "engine/studio_ui/src/StudioShellTerrainTools.cpp",
  "engine/studio_ui/src/StudioShellPersistence.cpp",
  "engine/studio_ui/src/StudioShellNavigation.cpp",
  "engine/studio_ui/src/StudioViewportPanels.cpp",
  "engine/studio_ui/src/StudioRenderViewSet.cpp",
  "engine/studio_ui/src/StudioRenderViewRpc.cpp",
  "engine/studio_ui/src/StudioShellInspector.cpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioShellModel.hpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioExpansionShell.hpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioInspectorExtension.hpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioViewContinuity.hpp",
  "engine/studio_ui/include/orbit/studio_ui/ViewportCaptureService.hpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioViewportCamera.hpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioViewportNavigation.hpp",
  "engine/studio_ui/include/orbit/studio_ui/ImplementationPlan.hpp",
  "engine/studio_ui/include/orbit/studio_ui/PlanningUi.hpp",
  "engine/studio_ui/src/ImplementationPlan.cpp",
  "engine/studio_ui/src/PlanningUi.cpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioFlatMap.hpp",
  "engine/studio_ui/include/orbit/studio_ui/ProfilerUi.hpp",
  "engine/studio_ui/include/orbit/studio_ui/ProfilerModel.hpp",
  "engine/studio_ui/src/ProfilerUi.cpp",
  "engine/studio_ui/src/ProfilerPanelRpc.cpp",
  "engine/studio_ui/src/ProfilerModel.cpp",
]
symbols = ["StudioExpansionShell", "StudioInspectorProviderRegistration", "StudioViewContinuity", "ViewportCaptureService", "ProfilerUi", "StudioTerrainNavigationConfig"]
invariants = [
  "Snap remains visible with Select active and opens the same settings editor used by Properties. Distance units default to meters and support mm/cm/m/km/in/ft; the manipulator still consumes canonical meters. Angle increments use degrees and scale uses percent. StudioViewportPanels::SetSnapping validates finite positive increments before changing shared state; viewport.snapping_get/set and MCP reuse it. Distance and preferred unit persist in StudioPersistentState, and old state defaults to meters. Native implementation changes use the central generation handoff.",
  "Build groups transform tools with smaller World/Local/Snap modifiers, categorized creation menus (Meshes, Procedurals, Lighting, VFX, SFX), selection editing, and history. The shell no longer reserves a selected-object breadcrumb row; command shortcuts and argument forms are serviced by the remaining context band. Categories gather registered creation commands and plugin contributions and use the existing owners, including argument forms for parameterized tools. Empty categories report that no tools are registered. Selected tools use an accent tint; unavailable creation, selection, and history commands are disabled. Universe capability toggles use the same presentation. Narrow layouts retain all actions as compact icons with tooltips, and UI callbacks retain the existing RPC-backed owners.",
  "StudioExpansionShell owns lifecycle and contribution registration; StudioShellPersistence, StudioShellInspector, StudioShellTerrainTools, StudioShellNavigation and StudioShellViewportBand implement its independent responsibilities. Navigation delegates command palette, quick-create popup and full argument/browser forms to separate implementation units. All share the same shell-owned state and existing UI/RPC authority.",
  "Built-in authoring tools and hot-reloadable plugins share ONE inspector provider registry and ONE Properties-panel extension owned by StudioExpansionShell, so two visually identical contextual-inspector pipelines cannot drift; a registration lives exactly as long as its authoring UI instance, and headless/model workflows may populate the registry without an EditorUi host.",
  "The left-side Explorer is one panel and one searchable tree for both world objects and project assets. StudioViewportPanels composes the internal Explorer Source into that panel; the old World/Assets mode switch is absent. The Build, Planet, Universe and Simulation workspaces share one spatial shell; Shading, Planning and Plugins switch the center surface.",
  "Right-clicking an authored Explorer row opens contextual Add element and Delete actions. Add choices follow the clicked parent type and call CommandService; created objects become selected. The world root is protected, and Delete is offered only for leaf objects because object.delete shares that invariant over RPC/MCP.",
  "The separate top workspace tab band is the single mode selector; Build maps to the existing Scene mode and Universe maps to Celestial, preserving old Scene/Celestial workspace RPC and persistence aliases.",
  "Planet has a dedicated generation toolbar rather than Build creation categories: create a rocky planet through the authoring command, toggle Surface/Atmosphere/Clouds-Volumetrics/Ocean/Rings/Aurora capabilities with inline element bubbles, open atmosphere presets and solve derived coefficients through AtmospherePropertySolver, choose terrain generation/editing tools, author the persisted deterministic spherical tectonic recipe in Tectonics, and edit runoff budget and river-network recipes, draw a downhill drainage-guidance spline, and preview drainage, precipitation and standing water through Hydrology. The tectonic map layer shows plate identity with convergent/divergent/transform boundary influence from the same analytic field used by terrain. Recipe edits use SurfaceAuthoringModel and terrain.tectonics_get/set, terrain.hydrology_get/set, terrain.rivers_get/set and terrain.drainage_spline_add RPC/MCP methods; map.layer_set supports the existing layers. The shared viewport band retains transform and snapping controls. Capability and terrain state use their existing owners and RPC/MCP routes.",
  "The workspace tab band uses icon-and-label NavigationTabs on an elevated neutral surface with a shadow gutter; the selected workspace alone has an accent underline. View-mode and surface-debug selectors have compact widths instead of stretching across the contextual band.",
  "PlanningUi is presentation only. Its project-local ImplementationPlan store is canonical for bubble title, description, status, canvas position and predecessor; the canvas and planning.* RPC/MCP call the same mutations.",
  "Keyboard-first '+ Add' reuses the existing command palette (filtering for authoring verbs surfaces Create/Add/New descriptors with generated argument forms and project-asset pickers) instead of a second modal.",
  "View continuity (camera pose and simulation time of the primary view, saved to <project>/.orbit/StudioView.ini) is presentation state only and never world authority; unknown keys are ignored and a pose with a missing or non-finite field is dropped whole rather than half-applied.",
  "Viewport navigation controls are presentation-only: nothing in them participates in terrain authority, generation revisions or cache identity; the viewport camera converter takes a generation-stamped logical target and REJECTS a stale universe target rather than rendering it against a replacement FrameGraph/BodyRegistry. Perspective navigation falls back to the view-owned free camera when no celestial target exists, so blank/new worlds remain navigable; viewport.navigate RPC/MCP uses the same path.",
  "High-resolution capture of the primary viewport: up to 8K the view is resized to the capture size and given a few frames to settle (temporal filtering, terrain streaming, lighting caches); larger shots (16K) do not fit in GPU memory as one render, so the camera is turned onto a grid of tiles with a narrower field of view and each tile is rendered at 4K.",
  "The flat planet map generates every layer from the same terrain samples (switching layers never re-samples the planet); its tectonics layer is populated from the same deterministic GlobalTerrainFields plate field and overlays convergence red, divergence cyan and transform yellow, blended by mask weight (oblique boundaries mix instead of flipping between two colours). Three layers separate what it mixes: plate_id (plate identity and outlines only), boundary_motion (convergence/divergence/shear only, dark where plates do not interact) and crustal_deformation (stress ramp with the fault network drawn over it), each from the same samples (SampleTectonicStructure for the last), so a feature can be told from an artifact of one view. It shares the HUD's convention: latitude = asin(direction.y) with +Y the spin pole, longitude = atan2(direction.z, direction.x), in degrees, on a 2:1 equirectangular image.",
  "Every profiler panel control has a profiler.panel_* RPC/MCP equivalent driving the same ProfilerModel (MCP parity).",
  "ProfilerModel owns viewport-only capture defaults (4 seconds, 1440p and static scenario); its panel controls and profiler.panel_get/set RPC/MCP share those options. Capture viewport only exposes a 1–30 second duration, 720p/1080p/1440p/2160p presets and repeatable static/walk/fast-surface/low-flight/ground-to-orbit camera scenarios. Its profiler.viewport_capture action shares the StudioApplication-owned lifecycle; EditorUi draws only the fullscreen primary viewport at the selected target size, applies camera-only motion through StudioRenderViewSet and restores the original pose, target size and shell when complete or canceled, then reports the path or error.",
  "The Explorer's protected Viewport Camera is a virtual view-owned item, never a world object; lens FOV/focal-length settings update StudioRenderViewSet's canonical per-view zoom, and its Eye Adaptation child edits StudioViewportRenderer's existing per-view config as an artistic camera control. Properties selections call those owners directly; view.camera_* RPC/MCP expose lens state and display.eye_* RPC/MCP expose eye settings.",
  "The authored world object (for example, GI Room World) is the visible Explorer root; protected Viewport Camera and Lighting items are nested beneath it, and there is no synthetic World Root row. Properties shows the selected object schema and only relevant type-specific providers; global workflow/navigation/preset/property utility controls do not appear as stale object properties.",
  "The Explorer's protected Lighting item groups renderer contributions into Global Illumination, Direct Lighting & Shadows, Reflections, Atmosphere, Clouds, Ocean & Surface, Anti-Aliasing and Renderer Diagnostics. Contribution controls edit StudioRenderViewSet layer options exposed by view.terrain_layers_* RPC/MCP; diagnostics reuse DisplayDiagnosticsUi. Eye Adaptation remains a Camera child and is omitted from the Lighting renderer body.",
]
related = ["/editor/viewport", "/editor/mcp-rpc", "/editor/model", "/editor/session", "/editor/reports", "/editor/studio-ui/planning", "/rules/ui", "/legacy/orbit-profiler", "/rendering/shading"]
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
verified = "329654cd3290274cd945e9daec3fd214267ef2c8"
[routes]
"Implementation planning bubbles, project persistence or planning MCP tools" = "/editor/studio-ui/planning"
"Celestial panel, Celestial Tools, atmosphere preset buttons, System View canvas or orbit handles misbehave" = "/editor/studio-ui/celestial-authoring"
"Project Browser, World Documents, Project Settings, create/open project or world" = "/editor/studio-ui/world-and-project"
"Surface panel cache, revision or rebuild numbers look wrong, or a Surface edit has no RPC" = "/editor/studio-ui/surface-authoring"
"Volumes panel, Representation/LOD, cache bake, particle output, or Shading tab panel misbehaves" = "/editor/studio-ui/volume-authoring"
"Report has no screenshot, or reports.restore leaves the camera or clock wrong" = "/editor/studio-ui/reports"
"Text HUD, view.text_diagnostics, eye adaptation, Debug panel or transport band misbehaves; a Display Diagnostics control has no RPC" = "/editor/studio-ui/diagnostics-hud"
"V0.0.7 validation commands, performance JSON, 16K captures, or what is still unvalidated" = "/editor/studio-ui/validation-and-capture"
+++

Area to file map (`engine/studio_ui/include/orbit/studio_ui`): shell and persistence - `StudioShellModel`, `StudioExpansionShell`, `StudioPersistentState`, `StudioViewContinuity`, `StudioUiContributions`; authoring panels - `ProjectAuthoringUi`, `ProjectSettingsUi`, `WorldDocumentsUi`, `CelestialAuthoringUi`, `SurfaceAuthoringUi`, `VolumeAuthoringUi`, `SimulationControlsUi`, `SystemViewUi`, `ShadingUi`; viewport - see `/editor/viewport`; diagnostics - `DisplayDiagnosticsUi`, `DisplayEyeRpc`, `StudioTextDiagnosticsHud`, `ProfilerUi`/`ProfilerModel`/`ProfilerPanelRpc`, `ReportsUi`; release evidence - `SceneValidationScenarios`, `V007ValidationCommands`.
