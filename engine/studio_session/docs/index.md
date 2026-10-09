+++
path = "/editor/studio-session"
title = "Studio session, terrain runtime bridge and viewport targets"
kind = "subsystem"
status = "stable"
summary = "The composition layer of Studio's runtime: StudioSession and runtime bindings, the terrain runtime bridge with its rebuild scheduler and physical-page service, the project browser model, the viewport target registry, the simulation clock binding and the production round-trip verification and optional tooling-owned terrain acceptance. The authoring binding, project settings model, workspace RPC host, Bezier handle editor and celestial validation scenarios were removed in 0.0.9 because no application linked them."
owner_module = "OrbitStudioSession"
keywords = ["studio session", "runtime binding", "terrain runtime bridge", "rebuild scheduler", "physical page service", "project browser", "viewport target", "workspace", "validation scenario"]
sources = [
  "engine/studio_session/include/orbit/studio_session/ProjectBrowserModel.hpp",
  "engine/studio_session/include/orbit/studio_session/SimulationClock.hpp",
  "engine/studio_session/include/orbit/studio_session/StudioRuntimeBinding.hpp",
  "engine/studio_session/src/StudioTerrainStatusRpc.cpp",
  "engine/studio_session/src/StudioTerrainSampleRpc.cpp",
  "engine/studio_session/src/StudioTerrainAuthoringInvalidation.cpp",
  "engine/studio_session/include/orbit/studio_session/StudioTerrainAuthoringInvalidation.hpp",
  "engine/studio_session/tests/StudioTerrainAuthoringInvalidationTests.cpp",
  "engine/studio_session/include/orbit/studio_session/StudioTerrainPhysicalPageService.hpp",
  "engine/studio_session/CMakeLists.txt",
]
symbols = ["RecentProjectItem", "SimulationClock", "StudioRuntimeSnapshot"]
invariants = [
  "BuildImpactHistoryInvalidations delegates local edits to ImpactField's ChangedAuthoredEventInfluenceCaps, preserving the union of moved/removed/added event bounds, long ray support and physical size scaling. It falls back to global physical-page invalidation if a changed cap exceeds the current 64-tile bounded scope, or history advection requires a global rebuild; it never silently clips a large impact influence.",
  "StudioTerrainValidationScenario is tooling-owned in tools/validation and compiled into OrbitTerrainValidationSupport only with BUILD_TESTING or ORBIT_ENABLE_VALIDATION_TOOLS. OrbitStudioSession never links back to this support library. Studio production UI includes it only with ORBIT_ENABLE_VALIDATION_TOOLS=ON.",
  "Terrain authoring edits flow through StudioTerrainAuthoringInvalidation into the dependency graph (/rendering/terrain/invalidation) and the rebuild scheduler; physical page uploads carry an upload revision (BeginUpload/CompleteUpload) so a stale completion can be rejected (/rendering/terrain/gpu-cache).",
  "Reading the ground never needs a camera: terrain.list, terrain.sample (batched points or a great-circle transect, optionally with the plate structure) and studio.process_info (pid, to detect a relaunch or hot handoff) are read-only RPC/MCP methods in StudioTerrainSampleRpc.cpp; viewport.fly_to (studio_ui) is the one-call way to place the camera.",
  "StudioSession registers its own RPC methods on the session dispatcher in its constructor: viewport-target methods (view.*), read-only terrain.cache_stats, terrain.tectonics_get/set, terrain.impacts_get/set (validated chronological .orbitimpacts scene recipe with impact, connected resurfacing and tectonic-renewal events plus bounded invalidation for changed local records and global invalidation for planetary population, age/environment or fracture changes), terrain.stratigraphy_get/set (validated .orbitstratigraphy material layers consumed during physical page builds), terrain.erosion_coupling_get/set (geological-age and tectonic drainage coupling on the terrain process settings), terrain.bake_status/start/cancel/set (planet bake, served by StudioTerrainBakeController, which also bakes before the first terrain page and installs finished bakes; bake status includes geology raster work counters), read-only terrain.tectonics_sample (structural layer at a point or under the observer, via ProbeTectonicStructure), terrain.processes_get/set, terrain.hydrology_get/set, terrain.drainage_spline_add, terrain.rivers_get/set, terrain.rivers_nearby and terrain.river_constraint_add. Terrain authoring calls delegate to SurfaceAuthoringModel so the Planet toolbar and MCP share persisted settings, transactional writes and bounded invalidation. rivers_nearby reads immutable M16 products from built physical-page snapshots around the viewport observer; river_constraint_add persists basin/page-local intent that the production physical-page builder captures and passes to M16.",
  "The immutable physical-page snapshot may publish the derived M16 river graph alongside M08 material and M29 debug products; the viewport may draw it as diagnostic geometry without changing terrain authority.",
  "StudioTerrainPhysicalPageService builds every physical page independently: pages never read a neighbour's solved drainage, publish boundary payloads, queue neighbour rebuilds or wait on each other (no page gate). When the terrain source carries a baked incision raster and the page has no authored height/uplift/protection/drainage constraints, BuildProcesses skips SolveStreamPowerErosion (the bake already applied it). BuildDrainageHalo samples the terrain source just outside the page, and that source already carries the baked river channels, so macro continuity across page seams comes from the bake rather than from an iterative exchange. After the page's local D8 solve, ApplyBakedRiverDischarge (BakedRiverPage.cpp) raises area and discharge inside the bankfull baked channel to the baked values. When a page has no authored river constraints, BuildProcesses sets its river network with BuildBakedPageRiverNetwork (the baked graph clipped to the page: nodes on stored centerlines, baked width/depth/discharge, no routing, meanders or incision); pages with authored constraints keep the local BuildRiverNetwork + incision solve.",
]
related = ["/editor/mcp-rpc", "/editor/session", "/editor/model", "/rendering/terrain/invalidation", "/rendering/terrain/gpu-cache", "/editor/viewport"]
depends_on = ["/authoring/documents", "/celestial/appearance", "/celestial/compact-objects", "/celestial/giants", "/celestial/lighting", "/celestial/magnetosphere", "/celestial/representation", "/celestial/small-bodies", "/editor/mcp-rpc", "/editor/model", "/editor/session", "/foundation/core", "/foundation/jobs", "/foundation/rpc", "/rendering/terrain/clipmaps", "/rendering/terrain/contracts", "/rendering/terrain/debug-fields", "/rendering/terrain/gpu-passes", "/rendering/terrain/hydrology", "/rendering/terrain/invalidation", "/rendering/terrain/macro-geology", "/rendering/terrain/material-column", "/rendering/terrain/scatter", "/rendering/terrain/streaming", "/rendering/volumes/representation", "/world/path-geometry", "/world/path-routing", "/world/paths", "/world/planet-coordinates", "/world/surface-composition", "/world/world-model"]
used_by = ["/apps/studio", "/editor/studio-ui"]
verify = [
  "ctest -R Orbit.StudioSession",
  "ctest -R Orbit.StudioRuntimeBinding",
  "ctest -R Orbit.StudioTerrainAuthoringInvalidation",
  "ctest -R Orbit.StudioTerrainRebuildScheduler",
  "ctest -R Orbit.StudioTerrainPhysicalPageService",
  "ctest -R Orbit.StudioTerrainWorldReopen",
  "ctest -R Orbit.StudioTerrainRuntimeBridge",
  "ctest -R Orbit.StudioWorkspace",
  "ctest -R Orbit.ProjectBrowserModel",
  "ctest -R Orbit.ViewportTargetRegistry",
  "ctest -R Orbit.SimulationClock",
]
verified = "707c225d50c50f01b9ef797e67903447c1a66d5f"
+++
