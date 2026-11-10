+++
path = "/rendering/terrain/clipmaps/debugging"
title = "Debugging clipmap terrain"
kind = "playbook"
status = "stable"
owner_module = "OrbitTerrainRender"
summary = """
Every clipmap failure has an instrument: terrain overlays (rings, level tint, sample health, hole view, projection \
view, shading view, wireframe, freeze), per-stage bypasses to bisect the frame, view.text_diagnostics for numbers, \
RenderDoc and Perfetto captures. Pick the playbook that matches the symptom, switch the overlay on over RPC/MCP, read \
the colour or number, and follow the link to the owning block."""
keywords = ["debug", "overlay", "diagnostics", "sample health", "hole view", "projection view", "shading view", "wireframe", "freeze", "bypass", "renderdoc", "clipmap_plan", "cracks", "holes", "missing triangles"]
sources = [
  "engine/studio_ui/src/StudioRenderViewRpc.cpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioTerrainDiagnosticOverlayGeometry.hpp",
  "engine/studio_ui/src/StudioTextDiagnosticsHud.cpp",
  "tools/mcp_server/orbit_editor_mcp_server.py",
]
symbols = ["orbit_view_terrain_overlays_set", "orbit_view_terrain_layers_set", "orbit_view_text_diagnostics"]
invariants = [
  "Overlays and layer toggles are per view; flags omitted from view.terrain_overlays_set / view.terrain_layers_set keep their value, so set only what you are testing.",
  "clipmap_hole_view, clipmap_projection_view and clipmap_shading_view cull nothing or recolour output; they are diagnostic only and must be switched off afterwards.",
  "bypass_* flags skip one frame stage for the view; bisect by toggling them one at a time and restore them to false.",
  "Clipmap statistics are only valid while the production terrain is drawn (below the orbital hand-off).",
  "While the clipmap is frozen only the terrain pass re-projects; sky, atmosphere and other passes use the live camera, so lighting is approximate away from the frozen centre.",
]
related = ["/rendering/terrain/clipmaps/level-planning", "/rendering/terrain/clipmaps/level-cross-fade", "/rendering/terrain/clipmaps/generator-parity"]
depends_on = ["/rendering/terrain/clipmaps", "/editor/mcp-rpc", "/editor/viewport"]
verify = [
  "After debugging, restore: every overlay false, clipmap_freeze false, bypass_* false (orbit_view_terrain_overlays_get / orbit_view_terrain_layers_get).",
]
verified = "04d589b3"

[routes]
"I need to see which levels are active" = "/rendering/terrain/clipmaps/level-planning"
"profile where the frame time goes" = "/legacy/orbit-profiler"

[[diagnose]]
symptom = "a strip or rows of terrain are one wrong colour or look corrupted"
steps = [
  "orbit_view_terrain_overlays_set(view_id, clipmap_sample_health=true): colours each vertex by what is wrong with its GPU sample.",
  "Read the colour: red = bad elevation, green = bad morph target, blue = bad slope, grey = healthy. Rows of one colour are a corrupted strip.",
  "Red/green strip after the camera moved: suspect the toroidal strip refresh (/rendering/terrain/clipmaps/lattice-and-tracking). Everywhere: suspect the generator (/rendering/terrain/clipmaps/generator-parity).",
]
docs = ["/rendering/terrain/clipmaps/lattice-and-tracking", "/rendering/terrain/clipmaps/generator-parity"]

[[diagnose]]
symptom = "holes or missing patches of terrain"
steps = [
  "orbit_view_terrain_overlays_set(view_id, clipmap_hole_view=true): culls nothing and colours each vertex by why it would be culled.",
  "Read the colour: red = beyond the horizon, green = inside a finer level's hole, blue = inside it while that level fades in, cyan = culled by distance bands, grey = drawn normally.",
  "Blue patches that never clear: the finer level's fade is stuck (/rendering/terrain/clipmaps/level-cross-fade). Green where no finer level exists: the plan's finest level lost its hole removal (ApplyClipmapActiveRange).",
]
docs = ["/rendering/terrain/clipmaps/level-cross-fade", "/rendering/terrain/clipmaps/level-planning"]

[[diagnose]]
symptom = "triangles are clipped away or the mesh disappears at some angles"
steps = [
  "orbit_view_terrain_overlays_set(view_id, clipmap_projection_view=true): culls nothing and colours each vertex by where its clip position lands.",
  "Read the colour: red = non-finite, green = behind the camera, blue = outside the near/far range, cyan = off screen sideways, grey = on screen.",
  "Red means NaN/inf in the vertex path (check the sample data first with clipmap_sample_health); blue means near/far planes versus altitude.",
]
docs = []

[[diagnose]]
symptom = "shading artefacts: flat colour, black or magenta pixels, wrong normals"
steps = [
  "orbit_view_terrain_overlays_set(view_id, clipmap_shading_view=true): colours each pixel by which interpolated shading input is bad, as emission.",
  "Read the colour: yellow = biome weights sum to zero, magenta = non-finite position, red/green/blue = terrain normal / body-fixed normal / surface direction bad, dark grey = fine.",
  "To tell terrain from lighting, bisect with orbit_view_terrain_layers_set bypass_indirect_lighting, bypass_atmosphere, bypass_cloud_shadow (one at a time).",
]
docs = []

[[diagnose]]
symptom = "I need to look at the rings and level hand-offs from outside"
steps = [
  "Switch on clipmap_rings (outlines only active levels), clipmap_levels (tints per level) and clipmap_wireframe (mesh as lines; water hidden).",
  "Switch on clipmap_freeze, then fly the camera away: the plan, level windows, residency and generated content stop following the camera. Unfreezing snaps back and re-plans.",
  "Read clipmap_plan.levels in view.text_diagnostics: level, spacing, half extent, drawn_vertices vs expected_vertices, fully_drawn (false = rows of that level are not submitted).",
]
docs = ["/rendering/terrain/clipmaps/level-planning"]

[[diagnose]]
symptom = "indirect lighting looks wrong or black in places"
steps = [
  "orbit_view_terrain_layers_set(view_id, indirect_coverage_view=true): replaces the final gather with its coverage: red confidence, green gathered brightness (log), magenta = the gather returned nothing so that pixel gets no indirect light.",
  "Bisect: bypass_indirect_lighting (final gather + hybrid reflections), bypass_hybrid_reflections (reflections only), bypass_radiance_cache (cache fallback only).",
]
docs = ["/rendering/lighting", "/rendering/lighting/sky-cache-fill", "/rendering/lighting/proxy-sun-shadow"]
+++

## Instruments

| Instrument | Tool / RPC | Tells you |
| --- | --- | --- |
| terrain overlays | `orbit_view_terrain_overlays_get/_set` -> `view.terrain_overlays_*` | rings, level tint, sample health, hole/projection/shading views, wireframe, freeze, cache status, drainage vectors... |
| terrain layers and bypasses | `orbit_view_terrain_layers_get/_set` -> `view.terrain_layers_*` | which layers draw, LOD bias, planner knobs, `bypass_*` stage skips, coverage views |
| numbers and HUD text | `orbit_view_text_diagnostics` -> `view.text_diagnostics` | camera/altitude, terrain/water at nadir and cursor, `clipmap_plan`, CPU terrain workers |
| GPU terrain cache | `orbit_terrain_cache_stats(terrain_id)` -> `terrain.cache_stats` | hits/misses/generations/evictions/resident pages and bytes; stationary-camera hit rate (`/rendering/terrain/gpu-cache`) |
| GPU capture | `orbit_renderdoc_capture` (Studio started with `ORBIT_RENDERDOC=1`) | a frame in RenderDoc |
| CPU/GPU timeline | `orbit_profiler_capture` | Perfetto trace of every thread |

The full parameter reference of every overlay and layer flag is in `docs/ORBIT_MCP.md`
(`/legacy/orbit-mcp/panels-and-the-shading-tab`).

## Method

1. State the symptom in one sentence and pick the playbook above.
2. Change **one** thing at a time (one overlay or one bypass) and restore it.
3. Record numbers (`clipmap_plan`, parity pair) rather than describing images.
4. Follow the playbook's `docs` links for the owning block and its invariants before editing code.
