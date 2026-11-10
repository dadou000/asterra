+++
path = "/rendering/terrain/rivers"
title = "River network, meanders and regional river carving (M16)"
kind = "subsystem"
status = "stable"
summary = "M16 builds river networks from M09 drainage, preserving confluences and stable basin IDs across resident cardinal page chains. Channels follow carved M08 terrain, feed the near-field water pass and publish removed sediment through M14."
owner_module = "OrbitTerrainErosion"
keywords = ["river", "river network", "meander", "cutoff", "centerline", "channel", "carving", "hydrology refinement", "regional elevation delta"]
sources = [
  "engine/terrain_erosion/include/orbit/terrain_erosion/RiverNetwork.hpp",
  "engine/terrain_erosion/include/orbit/terrain_erosion/RiverCarving.hpp",
  "engine/terrain_erosion/include/orbit/terrain_erosion/RegionalElevationDelta.hpp",
  "engine/terrain_erosion/include/orbit/terrain_erosion/HydrologyRefinement.hpp"]
symbols = ["BuildRiverNetwork", "RegionalElevationDeltaTerrainSource", "HydrologyRefinementConfig"]
invariants = [
  "M09 remains the authoritative derived drainage state (flow direction, conditioned elevation, area, discharge, page-boundary flow); M16 derives an editable/channel-evolution graph from it and does not replace it as the catchment/routing source.",
  "The drainage graph is the topological source of truth: meandering moves the physical channel centreline only inside a bounded corridor, and connectivity changes only through explicit cutoff events.",
  "M16 samples ordinary reaches up to the configured maximum graph-node spacing; headwaters, confluences, sharp bends, terminal cells and page exits remain explicit nodes. Nodes retain source-cell IDs, and sparse edges trace only along the authoritative M09 downstream path.",
  "Every qualifying tributary converging on the same downstream drainage cell shares one graph node; channel strength and width derive from accumulated contributing area and discharge.",
  "M16 graph nodes expose derived water level, local slope, Manning-estimated velocity, roughness, wetted cross-section area and M14 waterborne sediment inventory; segments expose path-average slope, velocity, roughness and sediment inventory. Sediment is a page-cell mass in kg, not concentration or transport rate.",
  "Ocean-height cells are explicit drainage outlets, so river routing can terminate at sea rather than treating each physical page edge as a basin outlet. Studio no longer builds this per page for baked planets: with a baked river graph and no authored constraints on the page, the page network is the baked graph clipped to the page (BuildBakedPageRiverNetwork), with basin ids taken from the baked mouth node, so one basin id spans every page a river crosses without any cross-page propagation. BuildRiverNetwork (meanders, cutoffs, thresholds, constraints) remains the solve for pages with authored river constraints and for sources without a bake.",
  "M16 emits a RiverBoundaryLink for a generated channel that exits its physical page. The Planet Hydrology toolbar and terrain.rivers_nearby expose each link's upstream node, basin, source page, direction and target edge cell; these are derived continuity records, not persisted nodes or proof that the destination page is resident.",
  "M17 LakeWater associates each M09 basin spill outlet with the first existing M16 node along the same page's downstream D8 path. This link is diagnostic only; M09 owns routing, M16 owns the graph, and no node is synthesized to force a connection.",
  "The immutable physical-page snapshot publishes its M16 network to viewport diagnostics and the near-field water-depth upload; water geometry is derived presentation and never becomes terrain authority.",
  "Physical terrain incision is baked only through M08 and removed river sediment is published through M14 waterborne sediment.",
  "The pre-M09 regional HydrologyGrid, RiverGraph and RiverCarvingField, plus RegionalElevationDelta and hydrology refinement, remain compatibility/regional tooling and are not the V0.0.4 M16 authority.",
  "Basin-local river intent is authored in the physical page's local metre frame (x east, y south) as an infinite local trajectory through a centre point."]
related = ["/rendering/terrain/hydrology", "/rendering/terrain/erosion", "/rendering/terrain/water"]
verify = [
  "ctest -R Orbit.TerrainRiverNetwork",
  "ctest -R Orbit.RegionalElevationDelta",
  "ctest -R Orbit.HydrologyRefinement"]
verified = "b0a0de7f"
+++
