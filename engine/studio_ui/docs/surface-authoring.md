+++
path = "/editor/studio-ui/surface-authoring"
title = "Surface authoring panel (relief, processes, biomes, cache and performance rows)"
kind = "subsystem"
status = "stable"
summary = "SurfaceAuthoringUi provides geology, stratigraphy, impact, fracture, process, biome and cache controls through shared authoring model and RPC/MCP operations."
owner_module = "OrbitStudioUi"
keywords = ["surface authoring", "surface tools", "terrain panel", "relief", "terrain processes", "biome", "biomes", "constraints", "surface cache", "cache stats", "M16 performance", "M30 reference", "M06 rebuild", "terrain.cache_stats", "orbit_terrain_cache_stats", "process settings invalidation", "SurfaceAuthoringModel"]
sources = [
  "engine/studio_ui/src/SurfaceAuthoringUi.cpp",
  "engine/studio_ui/src/StudioViewportPanels.cpp",
  "engine/studio_ui/include/orbit/studio_ui/SurfaceAuthoringUi.hpp",
  "engine/editor_model/include/orbit/editor_model/SurfaceAuthoringModel.hpp",
  "engine/world_model/include/orbit/world_model/WorldSchemas.hpp",
  "engine/world_model/src/WorldSchemas.cpp",
  "engine/terrain_geology/include/orbit/terrain_geology/Stratigraphy.hpp",
  "engine/studio_session/include/orbit/studio_session/StudioTerrainServiceStatus.hpp",
  "engine/studio_session/include/orbit/studio_session/StudioTerrainPerformanceDiagnostics.hpp",
  "engine/studio_session/src/StudioTerrainServiceStatus.cpp",
  "engine/studio_session/src/StudioTerrainPerformanceDiagnostics.cpp",
  "engine/studio_session/src/StudioTerrainStatusRpc.cpp",
  "engine/studio_session/src/StudioTerrainAuthoringInvalidation.cpp",
  "engine/studio_session/include/orbit/studio_session/StudioTerrainAuthoringInvalidation.hpp",
  "engine/studio_session/src/StudioTerrainPhysicalPageService.cpp",
  "engine/terrain_water/include/orbit/terrain_water/LakeWater.hpp",
  "tools/mcp_server/orbit_editor_mcp_server.py",
]
symbols = ["SurfaceAuthoringUi", "SurfaceAuthoringModel", "RelevantToSelection", "SetAutomationCoverageMode", "QueueTerrainInvalidation", "StudioTerrainStatusInspector", "StudioTerrainPerformanceDiagnostics", "StudioTerrainPerformanceSnapshot", "M30Reference", "RegisterStudioTerrainStatusRpc", "orbit_terrain_cache_stats", "orbit_terrain_lakes_nearby"]
invariants = [
  "The panel owns no terrain state: each Draw builds a SurfaceAuthoringModel from world.Objects(), world.Commands() and world.Selection(), and every edit is a model call (SetRelief, SetProcessSettings, AddBiome, SetCommonPreferences, SetBiomeSettings, PaintLocalOverride, AddSurfaceLayer, AddScatterRule); a thrown std::exception becomes the status line at the bottom of the panel instead of escaping.",
  "One Draw serves two hosts: the standalone panel (kPanel, title 'Surface Authoring', closed by default, docked right) and the contextual inspector provider 'orbit.surface-authoring' ('Surface Tools'); both show only when SelectedRockyBody() resolves, which includes terrain descendants (biomes, constraints, process records), so selecting a child never drops the user out of the workflow.",
  "The Planet toolbar's Tectonics menu is the primary authoring entry point for the persisted spherical plate recipe; SurfaceAuthoringModel owns validation and undoable writes, and terrain.tectonics_get/set call that same model. The menu has a Planet Bake section (bake state and progress, resolution, auto-rebake, Bake Now and Cancel) that calls the same StudioTerrainBakeController as terrain.bake_status/start/cancel/set; it reports the last geology compile's sample count, event records, updated tiles and sample throughput, and terrain generation samples the baked planet structure without evaluating plates. It also has a Geological Coupling section (age erodibility gain, age uplift decay, belts steer watersheds) persisted as terrain process settings and queued as a global process-settings invalidation, the same operation as terrain.erosion_coupling_get/set. It also shows a Structural Sample readout (plate, boundary, crust thickness and age, uplift, subsidence, stress, volcanism) under the observer or at a typed latitude/longitude, from the same ProbeTectonicStructure call as terrain.tectonics_sample. The Flat Map tectonics layer displays plate identity and convergent/divergent/transform boundary influence from the generated field. Macro uplift/drainage controls remain separate authored terrain fields.",
  "Geological Event History offers planet-aware airless-moon and icy-moon starter recipes plus Reload Saved. Drafts are retained per terrain surface while the user switches selection; examples and Reload Saved change only the draft. Save validates the recipe and planet ID, creates one undo step, then diffs stable event IDs and queues bounded TerrainAuthoring invalidation for changed crater/flow footprints. Procedural populations, global age/environment changes, fracture-network changes, and tectonic slips that advect older impacts invalidate the planet. Tectonic-renewal entries may set displacement_x/y/z and displacement_m to move older crater centers; plate_motion=true requires a closed spherical centerline and shifts the historical features inside it. terrain.impacts_get/set and orbit_terrain_impacts_get/set use the same model and invalidation builder. Authored impacts, connected resurfacing centerlines and ice-fracture stress settings remain canonical scene data.",
  "The Planet toolbar exposes Craters and Volcanology as focused editors over one per-terrain typed impact-history draft. Craters edits global target/environment settings, the procedural size-frequency distribution, and every runtime ImpactRecord parameter; stable IDs remain read-only. Volcanology edits LavaFlow resurfacing records, including width, thickness, age, chronology, enabled state and every spherical centerline point. Lunar maria are placed under Volcanology > Lava Flows / Maria and positioned with latitude/longitude points (+Y north, longitude zero along +X). Both editors save through SurfaceAuthoringModel::SetImpactHistoryToml and BuildImpactHistoryInvalidations, matching terrain.impacts_get/set RPC/MCP behavior; reload discards only that terrain's unsaved draft.",
  "The Planet toolbar's Ice & Cryosphere menu edits the same per-terrain impact-history draft: every IceFractureDefinition input (seed, chronology, tidal/spin axes and stress, strength, count, segmentation, dimensions and branching), plus IceRenewal resurfacing events with enabled state, age, width, thickness and spherical centerline. Fracture axes and renewal points use latitude/longitude. Save uses SurfaceAuthoringModel::SetImpactHistoryToml and BuildImpactHistoryInvalidations; terrain.impacts_get/set and MCP expose the same records.",
  "The Stratigraphy section offers a two-layer starter built from materials already available on the selected body plus Reload Saved. Drafts are retained per terrain surface while the user switches selection; examples and Reload Saved change only the draft. Save validates material references and the .orbitstratigraphy profile before queuing a planet material-column rebuild. Composition compiles the profile against that body's M02 geological material library; the physical page builder samples it at the terrain surface after authored impact excavation to choose exposed bedrock, so crater floors can reveal deeper layers. terrain.stratigraphy_get/set and dedicated MCP tools call the same model operation.",
  "The Planet Hydrology menu edits persisted rainfall, infiltration, soil moisture, evaporation and seasonal forcing. The adjacent Rivers menu edits the complete RiverNetworkConfig and generation switch through SetProcessSettings, previews drainage/channels, draws authored downhill guidance and inspects nearby generated river graphs and lakes. terrain.hydrology_get/set covers the runoff budget and simulation-time sinusoidal rainfall amplitude/period/phase; seasonal forcing scales the static precipitation field globally and updates drainage/M16 products in twelve bins per cycle, without latitude-dependent seasons, snowmelt or groundwater. terrain.rivers_get/set covers maximum graph-node spacing, drainage/discharge thresholds, discharge-based channel width/depth, meander behavior and cutoff geometry; both UI and RPC/MCP queue the same global process-settings invalidation. M16 sparsifies straight reaches while preserving headwaters, confluences, sharp bends and page exits; sparse edges still trace M09 flow and drive carving. M16 channels drive the existing near-field water pass. Rivers also shows generated nodes, routing segments, lake basin spill/downstream summaries and page continuations near the active viewport; terrain.rivers_nearby exposes the full graph and hydraulic summaries, and terrain.lakes_nearby exposes page-local depression and spill details. Missing pages are reported as pending. This association traces existing M09 flow; it does not synthesize a river through a lake or resolve a connection on another page. Attract, repel and trajectory buttons create persisted basin/page-local River Basin Constraint objects through SurfaceAuthoringModel; production M16 receives them only for their authored page. Hydrology retains precipitation and standing-water flat-map previews. Generated lake and river fields remain derived, and direct node dragging is not available. This remains regional drainage/river authoring, not global watersheds or a local fluid solver.",
  "The Surface Authoring Terrain Processes section and Planet toolbar's Terrain Processes menu edit the same stream-power, hydraulic, thermal, aeolian, glacial and coastal process settings through SetProcessSettings. The toolbar groups process enablement and common solver controls by process, with iteration/time-step controls in each disclosure. Edits queue global ProcessSettings invalidation. terrain.processes_get/set expose the same visible fields through RPC/MCP; Hydrology and Rivers retain their focused recipe controls and matching RPCs. Relief, tectonics, biome and constraint edits update their canonical semantic properties; normal composition and terrain revision tracking derive the resulting runtime updates.",
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
verified = "1243e15a"

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
| Geology | Planet toolbar's Tectonics menu edits the plate recipe and bake policy; this panel edits macro/detail relief and offers airless-moon/icy-moon history starters, a saved-draft reload, and direct `.orbitimpacts` editing | `Tectonics`, `SetTectonics`, `SetRelief`, `SetImpactHistoryToml` |
| Planet Craters toolbar menu | Shared geological history metadata and target properties; procedural crater count, radius range and size-frequency exponent; complete authored crater shape, location, degradation, chronology, ejecta, rays, impactor scaling, binary impact and secondary crater parameters | `ImpactHistoryToml`, `SetImpactHistoryToml`, `terrain.impacts_get/set` |
| Planet Volcanology toolbar menu | Lava resurfacing and lunar maria; enabled state, width, thickness, formation age, chronology and an editable spherical centerline using latitude/longitude points | `ImpactHistoryToml`, `SetImpactHistoryToml`, `terrain.impacts_get/set` |
| Stratigraphy | Two-layer example built from this body's geology materials, saved-draft reload, and direct `.orbitstratigraphy` editing | `SetStratigraphyToml` |
| Terrain Processes | enable flags and iteration counts for stream power, hydraulic, thermal, aeolian, glacial, river/meander, coastal; Advanced reveals exact solver fields; 'Select Process Record' | `SetProcessSettings` then `QueueTerrainInvalidation` |
| Planet Hydrology toolbar menu | Rainfall, infiltration, soil moisture, evaporation, seasonal amplitude/period/phase, plus precipitation and standing-water flat-map previews | `ProcessSettings`, `SetProcessSettings`, `StudioRenderViewSet::SetFlatMapLayer`, `terrain.hydrology_get/set` |
| Planet Rivers toolbar menu | River generation; every `RiverNetworkConfig` field; viewport drainage/channel overlay; authored drainage-guidance spline; nearby generated graph, lake and page-continuation inspection; basin-local attract/repel/trajectory constraints | `ProcessSettings`, `SetProcessSettings`, `AddDrainageSpline`, `AddRiverBasinConstraint`, `terrain.rivers_get/set`, `terrain.drainage_spline_add`, `terrain.rivers_nearby`, `terrain.lakes_nearby`, `terrain.river_constraint_add` |
| Planet Terrain Processes toolbar menu | Enable switches and common/advanced settings for stream-power, hydraulic, thermal, aeolian, glacial and coastal erosion; grouped by solver with progressive disclosure | `ProcessSettings`, `SetProcessSettings`, `terrain.processes_get/set` |
| Planet Ice & Cryosphere toolbar menu | Ice fracture-network stress, axes, age and geometry; Ice Renewal events with width, thickness, chronology and latitude/longitude centerlines | `ImpactHistoryToml`, `SetImpactHistoryToml`, `terrain.impacts_get/set` |
| Planet Explore & Edit toolbar menu | Open this full authoring panel and switch directly to any supported planetary flat-map layer | `EditorUi::FocusPanelByTitle`, `StudioRenderViewSet::SetFlatMapLayer`, `map.layer_set` |
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
| Geological event history (impacts, ejecta, resurfacing, tectonic renewal and ice fractures) | yes: `terrain.impacts_get/set` / `orbit_terrain_impacts_get/set` |
| Terrain process settings exposed in the Planet toolbar | yes: `terrain.processes_get/set` / `orbit_terrain_processes_get/set` |
| Stratigraphy profile | yes: `terrain.stratigraphy_get/set` / `orbit_terrain_stratigraphy_get/set` |
| Planet bake policy, status, start and cancel | yes: `terrain.bake_set/status/start/cancel` and dedicated MCP tools |
| Relief, process, biome, biome preference, mask, moss and tree controls | no dedicated surface RPC/MCP operation; existing buttons still call the shared authoring model and bounded invalidation |
| Revisions beyond cache stats, M06 page counters, M16 timings and M30 reference | displayed in the panel only |

The geological workflows added here have matching RPC/MCP operations. The remaining generic surface controls and panel-only diagnostics above are existing MCP-parity gaps; expose them through their current authoring/diagnostic owners when those workflows are extended.
