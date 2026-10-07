+++
path = "/editor/studio-ui/surface-authoring"
title = "Surface authoring panel (relief, processes, biomes, cache and performance rows)"
kind = "subsystem"
status = "stable"
summary = """
SurfaceAuthoringUi is the "Surface Authoring" panel and the contextual "Surface Tools" inspector section for a terrain-bearing \
body: Geology (base relief), Terrain Processes, Authored Terrain (constraint list), Biomes and a read-only Surface Cache / Debug \
section. Edits go through SurfaceAuthoringModel command transactions; the cache, revision and performance numbers are copies from \
studio_session (StudioTerrainStatusInspector and StudioTerrainPerformanceDiagnostics). Only the cache numbers have an RPC \
(terrain.cache_stats); the edits have no dedicated RPC or MCP tool."""
owner_module = "OrbitStudioUi"
keywords = ["surface authoring", "surface tools", "terrain panel", "relief", "terrain processes", "biome", "biomes", "constraints", "surface cache", "cache stats", "M16 performance", "M30 reference", "M06 rebuild", "terrain.cache_stats", "orbit_terrain_cache_stats", "process settings invalidation", "SurfaceAuthoringModel"]
sources = [
  "engine/studio_ui/src/SurfaceAuthoringUi.cpp",
  "engine/studio_ui/include/orbit/studio_ui/SurfaceAuthoringUi.hpp",
  "engine/editor_model/include/orbit/editor_model/SurfaceAuthoringModel.hpp",
  "engine/studio_session/include/orbit/studio_session/StudioTerrainServiceStatus.hpp",
  "engine/studio_session/include/orbit/studio_session/StudioTerrainPerformanceDiagnostics.hpp",
  "engine/studio_session/src/StudioTerrainServiceStatus.cpp",
  "engine/studio_session/src/StudioTerrainPerformanceDiagnostics.cpp",
  "engine/studio_session/src/StudioTerrainStatusRpc.cpp",
  "tools/mcp_server/orbit_editor_mcp_server.py",
]
symbols = ["SurfaceAuthoringUi", "SurfaceAuthoringModel", "RelevantToSelection", "SetAutomationCoverageMode", "QueueTerrainInvalidation", "StudioTerrainStatusInspector", "StudioTerrainPerformanceDiagnostics", "StudioTerrainPerformanceSnapshot", "M30Reference", "RegisterStudioTerrainStatusRpc", "orbit_terrain_cache_stats"]
invariants = [
  "The panel owns no terrain state: each Draw builds a SurfaceAuthoringModel from world.Objects(), world.Commands() and world.Selection(), and every edit is a model call (SetRelief, SetProcessSettings, AddBiome, SetCommonPreferences, SetBiomeSettings, PaintLocalOverride, AddSurfaceLayer, AddScatterRule); a thrown std::exception becomes the status line at the bottom of the panel instead of escaping.",
  "One Draw serves two hosts: the standalone panel (kPanel, title 'Surface Authoring', closed by default, docked right) and the contextual inspector provider 'orbit.surface-authoring' ('Surface Tools'); both show only when SelectedRockyBody() resolves, which includes terrain descendants (biomes, constraints, process records), so selecting a child never drops the user out of the workflow.",
  "Of the panel's edits only the Terrain Processes section queues terrain regeneration: after SetProcessSettings it calls session->QueueTerrainInvalidation with TerrainChangeKind::ProcessSettings and a global scope on the planet id, and it throws (shown in the status line) when the terrain object has no active runtime body or no spherical planet definition. Relief, biome and constraint edits in this file queue nothing themselves.",
  "Brush and spline terrain constraints and Biome Paint are created in a Perspective or Body Map viewport (StudioTerrainAuthoringTool), not here: the Authored Terrain section only lists constraints and selects one so its numeric fields are edited in Properties. The only mask the panel creates is the advanced 'Create Exact Replace Mask'.",
  "The Surface Cache / Debug section is read-only: StudioTerrainStatusInspector::Capture and TerrainPerformance().Capture copy values (header: diagnostics only, they never mutate authority). The panel passes a null scheduler to Capture, so rebuildSchedulerAttached is always false there and the 'M06 Rebuild' row always prints 'live physical scheduler not attached yet'; live page counters appear in the 'M06 pages' row of the M16 block, which reads session.TerrainPhysicalPages().",
  "The 'M30 reference' rows are constants returned by StudioTerrainPerformanceDiagnostics::M30Reference() (captured 2026-09-20 on an RTX 4090 at 129 resolution), not live measurements; treat them as a baseline to compare the live rows against.",
  "terrain.cache_stats (read-only, MCP orbit_terrain_cache_stats) returns terrain, viewport, cache{hits, misses, generations, insertions, evictions, resident_pages, resident_bytes, hit_rate_percent}, stationary{frames, cache_hits, cache_misses, hit_rate_percent}, physical_lod, semantic_revision and surface_source_revision. It matches the panel's 'M26 Cache' row (status.cacheStats) and the stationary part of its 'M26 cache' row; geology/biome revisions, process summary, base biome, frame CPU, M06 page counters, edit-to-Ready times and streaming totals are visible only in the panel.",
  "SetAutomationCoverageMode(true) forces the Advanced biome and process sections open so the real-device smoke gate renders every control; normal Studio leaves it off and keeps the user's disclosure choices.",
]
related = ["/editor/studio-ui", "/editor/studio-session", "/editor/model", "/editor/mcp-rpc", "/editor/viewport", "/rendering/terrain/gpu-cache", "/rendering/terrain/invalidation", "/rendering/terrain/biomes", "/rendering/terrain/erosion"]
depends_on = ["/editor/studio-session", "/editor/model"]
used_by = ["/editor/studio-ui"]
verify = [
  "Call terrain.cache_stats for the selected terrain object and compare cache.resident_pages and cache.hits with the panel's 'M26 Cache' row.",
  "Open Surface Cache / Debug, edit a process iteration count, and confirm the status line reads 'Terrain process settings updated; M27 process descendants queued.'",
]
verified = "55d48117"

[routes]
"terrain cache misses or generations keep growing while the camera is still" = "/rendering/terrain/gpu-cache"
"edited process settings but the terrain did not regenerate" = "/rendering/terrain/invalidation"
"biome weights or biome paint look wrong" = "/rendering/terrain/biomes"
"need to create or move a terrain constraint brush" = "/editor/viewport"

[[diagnose]]
symptom = "Surface panel says 'M06 Rebuild: live physical scheduler not attached yet' while terrain is clearly rebuilding"
steps = [
  "This row is expected in this panel: SurfaceAuthoringUi passes a null scheduler to StudioTerrainStatusInspector::Capture, so it can never show page counters.",
  "Read the 'M06 pages' and 'Selected page' lines of the 'M16 Performance / Responsiveness' block instead; they come from session.TerrainPhysicalPages().",
  "If those also stay at zero, check that a terrain viewport is bound: 'Selected Physical Page: no terrain viewport bound.' means session.TerrainRuntime() gave no runtime for the body (the panel prefers viewport studio.primary).",
]
docs = ["/rendering/terrain/invalidation", "/editor/studio-session"]

[[diagnose]]
symptom = "Surface panel only prints 'Select a terrain-bearing Celestial Body, Terrain Surface, or one of its authored surface children.'"
steps = [
  "Select the planet, its Terrain Surface or a child record in the Studio browser; SelectedRockyBody() must resolve to a terrain object.",
  "A planet without a Terrain Surface: call world.ensure_planet_surfaces (MCP orbit_world_ensure_planet_surfaces), then reselect.",
  "'No live TerrainBodyServices are composed for the selected Terrain Surface.' inside the cache section means the body has no composed services yet (BodyForTerrainObject or ServicesForBody returned nothing).",
]
docs = ["/editor/model"]

[[diagnose]]
symptom = "need the surface cache hit rate without opening Studio's panel"
steps = [
  "Call terrain.cache_stats with {terrain: <terrain object id>, viewport: 'studio.primary'} (MCP orbit_terrain_cache_stats).",
  "Hold the camera still, call it twice: hits may grow, but stationary.cache_misses and cache.generations must not (see the gpu-cache block).",
  "Numbers the RPC does not return (frame CPU, M06 page counters, edit-to-Ready, streaming totals) must be read from the panel.",
]
docs = ["/rendering/terrain/gpu-cache"]
+++

## What it edits

All sections act on the selected terrain surface (`Planet: <body>`), through `editor_model::SurfaceAuthoringModel`, which wraps each change in a command transaction.

| Section | Controls | Model call |
| --- | --- | --- |
| Geology | macro/detail amplitude and wavelength, detail octaves, maximum elevation; read-only list of implemented geology systems and the count of semantic geology assets | `SetRelief` |
| Terrain Processes | enable flags and iteration counts for stream power, hydraulic, thermal, aeolian, glacial, river/meander, coastal; Advanced reveals exact solver fields; 'Select Process Record' | `SetProcessSettings` then `QueueTerrainInvalidation` |
| Authored Terrain | constraint list (Height, Protection, Drainage, Geology; Brush or Spline) with selection | `SelectObject` only |
| Biomes | Base Biome (non-removable residual fallback), add biome, automatic preference bands (temperature, moisture, elevation), material influence, scatter density, `+ Moss Surface`, `+ Tree Scatter`, advanced process multipliers and an exact replace mask | `AddBiome`, `SetCommonPreferences`, `SetBiomeSettings`, `AddSurfaceLayer`, `AddScatterRule`, `PaintLocalOverride` |

Process input is validated before the edit: `InputU32` rejects counts below 1 or above uint32 max and shows the message in the status line, and no model call is made in that frame.

## Surface Cache / Debug rows

Counts (`Semantic Revision`, terrain surfaces, biome services/definitions, universe bodies) come from `world`. Everything under "Live TerrainBodyServices" is `StudioTerrainStatusInspector::Capture(session, terrain, nullptr, "studio.primary")`: authority revisions (semantic, surface source, geology, biome), terrain source revision and physical revision fingerprint when available, default bedrock, exposed surface of the selected page, base biome and optional biome count, the process iteration summary, the `M26 Cache` row and the selected physical page, face, level and physical LOD. The `M16 Performance / Responsiveness` block is `session.TerrainPerformance().Capture(session, "studio.primary")`: build, CPU and GPU names, frame CPU last/avg/max (also during regeneration), M06 page counters, selected page state and edit-to-Ready, cache hit rates (overall and stationary), viewport streaming and the constant M30 reference.

## RPC / MCP coverage

| Panel operation | Reachable over RPC/MCP? |
| --- | --- |
| Cache stats, stationary counters, physical LOD, semantic and surface-source revision | yes: `terrain.cache_stats` / `orbit_terrain_cache_stats` |
| Terrain overlays that draw constraints, biome weights, process masks | yes: `view.terrain_overlays_set` (flags `authored_constraints`, `biome_weights`, `process_masks`) |
| Make sure planets have a Terrain Surface | yes: `world.ensure_planet_surfaces` |
| Generic object and property access (`object.get`, `object.create`, `property.set`, `command.invoke`) | yes, as raw schema access; none of them is a Surface-specific tool |
| Relief, process, biome, biome preference, mask, moss and tree buttons | no dedicated RPC or MCP tool was found; `QueueTerrainInvalidation(s)` is called from the panel, the viewport tools (StudioViewportPanelsBase.cpp) and the studio validation scenario, and no RPC handler found calls it |
| Revisions beyond those two, M06 page counters, M16 timings, M30 reference | no |

Under the MCP-parity rule in AGENTS.md these are gaps: extract the operation the button runs, register an RPC for it, add a tool in `tools/mcp_server/orbit_editor_mcp_server.py` and document it in `docs/ORBIT_MCP.md` in the same change.
