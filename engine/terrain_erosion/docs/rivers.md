+++
path = "/rendering/terrain/rivers"
title = "River network, meanders and regional river carving (M16)"
kind = "subsystem"
status = "stable"
summary = "The editable river-network layer on top of M09 drainage: a directed graph extracted from a DrainagePage whose centrelines may move inside a bounded corridor and change connectivity through explicit cutoff events, baking channel incision through M08 and publishing removed sediment as waterborne M14 sediment. The older regional river carving and hydrology refinement remain as regional tooling."
owner_module = "OrbitTerrainErosion"
keywords = ["river", "river network", "meander", "cutoff", "centerline", "channel", "carving", "river carved terrain", "hydrology refinement", "regional elevation delta"]
sources = [
  "engine/terrain_erosion/include/orbit/terrain_erosion/RiverNetwork.hpp",
  "engine/terrain_erosion/include/orbit/terrain_erosion/RiverCarving.hpp",
  "engine/terrain_erosion/include/orbit/terrain_erosion/RiverCarvedTerrainSource.hpp",
  "engine/terrain_erosion/include/orbit/terrain_erosion/RegionalElevationDelta.hpp",
  "engine/terrain_erosion/include/orbit/terrain_erosion/HydrologyRefinement.hpp",
]
symbols = ["BuildRiverNetwork", "RiverCarvedTerrainSource", "RegionalElevationDeltaTerrainSource", "HydrologyRefinementConfig"]
invariants = [
  "M09 remains the authoritative derived drainage state (flow direction, conditioned elevation, area, discharge, page-boundary flow); M16 derives an editable/channel-evolution graph from it and does not replace it as the catchment/routing source.",
  "The drainage graph is the topological source of truth: meandering moves the physical channel centreline only inside a bounded corridor, and connectivity changes only through explicit cutoff events.",
  "Physical terrain incision is baked only through M08 and removed river sediment is published through M14 waterborne sediment.",
  "The pre-M09 regional HydrologyGrid, RiverGraph, RiverCarvingField and RiverCarvedTerrainSource, plus RegionalElevationDelta and hydrology refinement, remain compatibility/regional tooling and are not the V0.0.4 M16 authority.",
  "Basin-local river intent is authored in the physical page's local metre frame (x east, y south) as an infinite local trajectory through a centre point.",
]
related = ["/rendering/terrain/hydrology", "/rendering/terrain/erosion", "/rendering/terrain/water", "/legacy/v0-0-4-m16-river-network"]
verify = [
  "ctest -R Orbit.TerrainRiverNetwork",
  "ctest -R Orbit.RegionalElevationDelta",
  "ctest -R Orbit.HydrologyRefinement",
]
verified = "b0a0de7f"
+++


