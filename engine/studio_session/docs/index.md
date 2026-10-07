+++
path = "/editor/studio-session"
title = "Studio session, terrain runtime bridge and viewport targets"
kind = "subsystem"
status = "stable"
summary = "The composition layer of Studio's runtime: StudioSession and runtime bindings, the terrain runtime bridge with its rebuild scheduler and physical-page service, the project browser model, the viewport target registry, the simulation clock binding and the terrain validation/round-trip scenarios. The authoring binding, project settings model, workspace RPC host, Bezier handle editor and celestial validation scenarios were removed in 0.0.9 because no application linked them."
owner_module = "OrbitStudioSession"
keywords = ["studio session", "runtime binding", "terrain runtime bridge", "rebuild scheduler", "physical page service", "project browser", "viewport target", "workspace", "validation scenario"]
sources = [
  "engine/studio_session/include/orbit/studio_session/ProjectBrowserModel.hpp",
  "engine/studio_session/include/orbit/studio_session/SimulationClock.hpp",
  "engine/studio_session/include/orbit/studio_session/StudioRuntimeBinding.hpp",
  "engine/studio_session/src/StudioTerrainStatusRpc.cpp",
  "engine/studio_session/CMakeLists.txt",
]
symbols = ["RecentProjectItem", "SimulationClock", "StudioRuntimeSnapshot"]
invariants = [
  "Terrain authoring edits flow through StudioTerrainAuthoringInvalidation into the dependency graph (/rendering/terrain/invalidation) and the rebuild scheduler; physical page uploads carry an upload revision (BeginUpload/CompleteUpload) so a stale completion can be rejected (/rendering/terrain/gpu-cache).",
  "StudioSession registers its own RPC methods on the session dispatcher in its constructor: the viewport-target methods (view.*) and the read-only terrain.cache_stats (StudioTerrainStatusRpc.cpp, a thin reader over StudioTerrainStatusInspector and StudioTerrainPerformanceDiagnostics that never mutates terrain state).",
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
verified = "04d589b3"
+++


