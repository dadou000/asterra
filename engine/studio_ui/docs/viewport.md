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
  "engine/studio_ui/include/orbit/studio_ui/StudioRenderViewSet.hpp",
  "engine/studio_ui/src/StudioRenderViewSet.cpp",
  "engine/studio_ui/src/StudioViewportPanels.cpp",
  "engine/studio_ui/src/StudioRenderViewRpc.cpp",
  "engine/studio_ui/src/StudioTextDiagnosticsHud.cpp",
  "engine/studio_ui/src/StudioViewportManipulatorUi.cpp",
]
symbols = ["StudioRenderViewSet", "NavigateTerrain", "StudioViewportPanels", "StudioTextDiagnosticsHud"]
invariants = [
  "The rendered camera (RenderView::Camera()) is output, rebuilt by StudioRenderViewSet::Refresh() from the navigation/viewport pose; change the upstream pose, never only the transient camera.",
  "Panels request operations; they do not own camera, terrain or renderer state.",
  "Every Diagnostics toggle in the viewport panel has an equivalent view.* RPC method and MCP tool (same flags, same defaults).",
  "Terrain diagnostic stats (clipmap_plan, etc.) are only valid while the production terrain is drawn.",
  "The Scene toolbar's Move / Rotate / Scale tools draw handles over the perspective viewport through StudioViewportPanels::HandleViewportGizmo, called right after the viewport Image; while the handles own the left button the press must not also select, pick terrain or place a path node (/editor/viewport/transform-gizmo).",
]
related = ["/editor/viewport/transform-gizmo", "/editor/mcp-rpc", "/rules/placement", "/rendering/terrain/clipmaps/debugging"]
depends_on = ["/rendering/terrain/clipmaps"]
verify = ["view.text_diagnostics returns the same text the HUD shows (its `text` field)."]
verified = "b0a0de7f"

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
