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
  "engine/studio_session/include/orbit/studio_session/StudioTerrainPhysicalPageService.hpp",
  "engine/studio_session/CMakeLists.txt",
]
symbols = ["RecentProjectItem", "SimulationClock", "StudioRuntimeSnapshot"]
invariants = [
  "StudioTerrainValidationScenario is tooling-owned in tools/validation and compiled into OrbitTerrainValidationSupport only with BUILD_TESTING or ORBIT_ENABLE_VALIDATION_TOOLS. OrbitStudioSession never links back to this support library. Studio production UI includes it only with ORBIT_ENABLE_VALIDATION_TOOLS=ON.",
  "Terrain authoring edits flow through StudioTerrainAuthoringInvalidation into the dependency graph (/rendering/terrain/invalidation) and the rebuild scheduler; physical page uploads carry an upload revision (BeginUpload/CompleteUpload) so a stale completion can be rejected (/rendering/terrain/gpu-cache).",
  "StudioSession registers its own RPC methods on the session dispatcher in its constructor: viewport-target methods (view.*), read-only terrain.cache_stats, terrain.tectonics_get/set, terrain.erosion_coupling_get/set (geological-age and tectonic drainage coupling on the terrain process settings), read-only terrain.tectonics_sample (structural layer at a point or under the observer, via ProbeTectonicStructure), terrain.hydrology_get/set, terrain.drainage_spline_add, terrain.rivers_get/set, terrain.rivers_nearby and terrain.river_constraint_add. Terrain authoring calls delegate to SurfaceAuthoringModel so the Planet toolbar and MCP share persisted settings, transactional writes and bounded invalidation. rivers_nearby reads immutable M16 products from built physical-page snapshots around the viewport observer; river_constraint_add persists basin/page-local intent that the production physical-page builder captures and passes to M16.",
  "The immutable physical-page snapshot may publish the derived M16 river graph alongside M08 material and M29 debug products; the viewport may draw it as diagnostic geometry without changing terrain authority.",
  "StudioTerrainPhysicalPageService keeps immutable M09 snapshots for resident pages, exchanges cardinal boundary payloads, and queues DrainageBoundary rebuilds only for resident neighbors whose exported boundary fingerprint changed. BuildDrainageHalo supplies the two-layer shared-edge halo (neighbor cell beyond the edge plus the neighbor's twin copy of the edge). The service installs a StudioTerrainRebuildScheduler page gate so an edge-adjacent page is not submitted while a neighbor is queued, building, or finished but not yet published; non-adjacent pages still build in parallel, which also makes the build order independent of completion timing.",
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
verified = "db348ce94035630577b705cffe0c69c6f8a6061f"
+++
