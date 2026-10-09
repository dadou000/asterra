+++
path = "/editor/viewport"
title = "Studio viewport, navigation and diagnostics"
kind = "subsystem"
status = "stable"
owner_module = "OrbitStudioUi"
summary = """
A viewport is a StudioRenderViewSet entry: navigation state (the canonical camera pose) feeds a \
RenderView that is rebuilt every frame by Refresh(). StudioViewportPanels draws toolbars and \
Diagnostics sections; view.* RPC methods (StudioRenderViewRpc) reach the same state, including \
terrain overlays, terrain layers and the text diagnostics HUD."""
keywords = ["viewport", "render view", "navigation", "camera", "diagnostics", "overlay", "hud", "text diagnostics", "refresh"]
sources = [
  "engine/studio_ui/src/StudioViewportVolumePass.cpp",
  "engine/studio_ui/src/StudioViewportLightingPass.cpp",
  "engine/studio_ui/src/StudioViewportBodyPass.cpp",
  "engine/studio_ui/src/StudioViewportAtmospherePass.cpp",
  "engine/studio_ui/src/StudioViewportCamera.cpp",
  "engine/studio_ui/src/StudioViewportCompose.cpp",
  "engine/studio_ui/src/StudioViewportDisplaySettings.cpp",
  "engine/studio_ui/src/StudioViewportNavigation.cpp",
  "engine/studio_ui/src/StudioViewportOverlayGeometry.cpp",
  "engine/studio_ui/src/StudioViewportPostProcess.cpp",
  "engine/studio_ui/src/StudioViewportPresentation.cpp",
  "engine/studio_ui/src/StudioViewportQuickCreate.cpp",
  "engine/studio_ui/src/StudioViewportRenderUtilities.cpp",
  "engine/studio_ui/src/StudioViewportRenderer.cpp",
  "engine/studio_ui/src/StudioViewportSceneResolution.cpp",
  "engine/studio_ui/src/StudioViewportTerrainPass.cpp",
  "engine/studio_ui/src/StudioViewportTerrainResources.cpp",

  "engine/studio_ui/include/orbit/studio_ui/StudioRenderViewSet.hpp",
  "engine/studio_ui/src/StudioRenderViewSet.cpp",
  "engine/studio_ui/src/StudioViewportPanels.cpp",
  "engine/studio_ui/src/StudioRenderViewRpc.cpp",
  "engine/studio_ui/src/StudioTextDiagnosticsHud.cpp",
  "engine/studio_ui/src/StudioViewportManipulatorUi.cpp",
]
symbols = ["StudioRenderViewSet", "NavigateTerrain", "StudioViewportPanels", "StudioTextDiagnosticsHud"]
invariants = [
  "StudioViewportRenderer coordinates frame graph composition; scene resolution, overlays, terrain resources, body rendering, the production terrain pass, lighting, atmosphere/clouds, volumes and display post-processing, display settings, celestial presentation state and the Compose wrapper are separate implementation units with one renderer owner. StudioScenePassContext borrows frame inputs synchronously; deferred render-graph callbacks copy values and resource owners rather than retaining the context.",
  "These implementation units remain in OrbitStudioUi and follow the central engine/apps native generation fallback; no reload host, new authoritative state or GPU retirement boundary is introduced by the split.",
  "The rendered camera (RenderView::Camera()) is output, rebuilt by StudioRenderViewSet::Refresh() from the navigation/viewport pose; change the upstream pose, never only the transient camera.",
  "Panels request operations; they do not own camera, terrain or renderer state.",
  "Every Diagnostics toggle in the viewport panel has an equivalent view.* RPC method and MCP tool (same flags, same defaults).",
  "Terrain diagnostic stats (clipmap_plan, etc.) are only valid while the production terrain is drawn.",
  "The Scene toolbar's Move / Rotate / Scale tools draw handles over the perspective viewport through StudioViewportPanels::HandleViewportGizmo, called right after the viewport Image; while the handles own the left button the press must not also select, pick terrain or place a path node (/editor/viewport/transform-gizmo).",
  "The terrain-derived near plane (0.2% of altitude above terrain) is clamped to a quarter of the distance to the nearest static mesh's oriented bounding box (floor 2 cm), so a camera hovering close to an imported mesh never clips it even when the terrain query reports a large altitude.",
  "The editor UI records profiler scopes: Ui.BeginFrame / Ui.NewFrame, Ui.shell.* sections, one `Panel <title>` per drawn panel and Ui.ensure / Ui.verts / Ui.upload / Ui.record inside EditorUi::Render; use them before guessing which panel costs frame time.",
  "The view layer setting render_scale (0.25..1, default 1; UI slider in the viewport Lighting/Renderer properties, view.terrain_layers_get/_set `render_scale`, MCP orbit_view_terrain_layers_set) makes the primary view render at that fraction of the panel size per axis and shows it stretched to the panel (ImageFit). Costs that scale with pixels shrink with it (about 0.62 ms of GPU per 0.1 scale at 4K in the atrium); geometry-bound passes (terrain, water, particles) do not. viewport.screenshot returns the render-resolution buffer.",
  "The volume-particle pass (simulation step and draw) is only recorded within 30 s of the last spawn event; the output generation advances every frame even with no events, so it cannot be used as the 'particles exist' signal.",
  "The near-field water pass is skipped when no screen ray can hit the sea sphere: from outside it, only rays steeper than the horizon dip (sin dip = sqrt(1 - (R/r)^2), 5 degree margin) can, so looking up or level from a high vantage records no water pass. It does not know about terrain occlusion, so it still runs when the sea is geometrically reachable but hidden.",
]
related = ["/editor/viewport/transform-gizmo", "/editor/mcp-rpc", "/rules/placement", "/rendering/terrain/clipmaps/debugging"]
depends_on = ["/rendering/terrain/clipmaps"]
verify = ["view.text_diagnostics returns the same text the HUD shows (its `text` field)."]
verified = "1229ef74"

[routes]
"frame the selection with the camera" = "frame-selected"
"terrain overlay or text HUD does not show what I expect" = "/rendering/terrain/clipmaps/debugging"
+++

## Where things live

| Concern | Owner |
| --- | --- |
| camera pose, navigation, per-view terrain settings | `StudioRenderViewSet` (`NavigateTerrain`, `Refresh`) |
| toolbars, Diagnostics properties | `StudioViewportPanels` (`DrawSceneToolbar`, ...) |
| RPC for views (`view.terrain_overlays_set`, `view.terrain_layers_set`, `view.text_diagnostics`) | `StudioRenderViewRpc.cpp` |
| HUD text | `StudioTextDiagnosticsHud.cpp` |
| keyboard shortcuts | `StudioQol.inl` |

## Reading the live state

`view.text_diagnostics` returns the numbers behind the HUD (camera position, altitude above datum,
terrain and water, coordinates, climate/biome weights, physical page/LOD, CPU terrain worker state and,
for clipmaps, `clipmap_plan`). MCP tool: `orbit_view_text_diagnostics`.
