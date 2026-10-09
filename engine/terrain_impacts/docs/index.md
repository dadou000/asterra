+++
path = "/rendering/terrain/impacts"
title = "Impact craters (M07)"
kind = "subsystem"
status = "stable"
summary = "ImpactField indexes impact, resurfacing and tectonic-renewal events and composes them chronologically. It generates scaled craters, ejecta, melt, breccia, age channels and microcrater residuals; IceFractureField generates stress-guided crack curves with groove, ridge and damage outputs."
owner_module = "OrbitTerrainImpacts"
keywords = ["impact", "crater", "ejecta", "size frequency distribution", "sfd", "excavation", "rays", "moon", "regolith", "ImpactRecord"]
sources = [
  "engine/terrain_impacts/include/orbit/terrain_impacts/ImpactField.hpp",
  "engine/terrain_impacts/src/ImpactField.cpp",
  "engine/terrain_impacts/src/ImpactFieldToml.cpp",
  "engine/terrain_impacts/tests/ImpactFieldTests.cpp",
  "engine/terrain_impacts/CMakeLists.txt"]
symbols = ["ImpactField", "ImpactFieldDefinition", "ImpactRecord", "ResurfacingRecord", "GeologicalEventKind", "GeologicalEventReference", "PreparedImpactGeometry", "CraterSizeFrequencyDistribution", "CraterProcessSample", "IceFractureField", "IceFractureDefinition", "IceFractureSegment", "ScaleImpactCraterRadiusMeters"]
invariants = [
  "Craters are process state, not biome decoration and not protected height stamps; generated crater terrain is disposable derived state.",
  "Procedural impacts follow a cumulative power law N(>R) ~ R^-b defined by count, minimum radius, maximum radius and exponent; radius sampling is deterministic from the planet generation seed and impact ordinal.",
  "Up to ten million procedural impacts are represented with a bounded 100,000-event explicit tier and an age-conditioned statistical micro-impact residual; smaller events do not become millions of heap objects.",
  "Placement uses deterministic equal-area sphere sampling and never cube-face UVs, clipmap rings, viewport state or cache slots; stable ImpactIds derive from seed plus ordinal so regeneration reproduces identity as well as placement.",
  "Explicit crater placements persist as .orbitimpacts project-authority records.",
  "Impact and connected-event influence are queried through separate conservative spherical-direction hierarchies, then local candidates are merged by ageOrder and stable event ID before one oldest-to-youngest composition pass. Younger excavation attenuates older relief, deposits and material cover; TectonicRenewal produces a fault ridge/trough, partial disruption and breccia while preserving formation age and exposed bedrock identity. A TectonicRenewal may author a slip direction and displacement; the canonical ImpactField transforms prepared centers of older impacts through later indexed fault events before both CPU and GPU compilation. `plate_motion=true` requires a closed spherical boundary and moves older structures inside it as one regional plate, sharing the same event record, chronology and bake path.",
  "ImpactField::ChronologicalEvents exposes stable references into the canonical resolved-impact and resurfacing arrays, sorted by age order and event ID with the event kind retained; regional compilers can pack only tile-intersecting records without creating a competing event authority.",
  "ImpactField::EventsIntersectingCap conservatively queries both spherical event hierarchies with a cap expanded by each node's maximum influence chord, then merges candidates chronologically; unrelated regional event batches avoid scanning the planet-wide event list.",
  "Regional compilers can call CollectEventsIntersectingCap with caller-owned ImpactQueryScratch and output storage, retaining vector capacity across tiles instead of allocating a candidate list for every region. The batched Sample overload accepts that conservative tile batch and preserves the same per-event exact influence tests and chronology while skipping a spatial-tree traversal for every texel.",
  "PreparedImpactGeometries is an immutable, index-aligned array of impact frames, phases and ejecta normalization coefficients built once with ImpactField; both the CPU raster reference and GpuGeologyCompiler pack these derived values instead of rebuilding them per sample or tile.",
  "Formation and exposure age orders are derived sample channels; authored event data remains the authority.",
  "Connected resurfacing polylines are project-authority events; the flow footprint crosses page/cube boundaries continuously, attenuates older crater channels, deposits a loose cover layer and resets local exposure/formation age by chronology.",
  "Ice fracture definitions carry formation age and event order; geology compilation attenuates fractures buried by younger crater excavation or connected resurfacing and advances exposure age when a fracture network forms later.",
  "Bake workers may retain ImpactQueryScratch across samples to reuse candidate and traversal storage without mutating the shared field.",
  "Crater size scaling includes gravity and target strength terms; optional event fields remain backward compatible in .orbitimpacts data. Tectonic displacement fields default to zero when reading older records. Changing or inserting a nonzero tectonic slip invalidates the full geology raster because it can move earlier features across regional boundaries in that history.",
  "Each resolved impact precomputes its tangent frame, stable angular phase and ejecta normalization factor. The bake sample reuses these constants instead of rebuilding the frame and hashing the event ID per texel; ejecta mass balance integrates bowl, rim, rebound and ray-modulated deposit profiles once in dimensionless polar area. This is an approximate local volume balance, with a bounded scale for numerically stable extreme recipes.",
  "ImpactField::kAlgorithmVersion participates in the terrain source recipe hash so hot-iterated changes to impact evaluation invalidate derived terrain and GEO1 products.",
  "Ice fracture curves are deterministic from tidal/spin axes and seed, spatially indexed, and sampled in planet directions so cube-face storage does not affect their paths.",
  "IceFractureField exposes its immutable generated segments and CollectSegmentsIntersectingCap reuses the same conservative hierarchy and caller-owned scratch to build tile-local fracture batches without scanning the full curve network. Cap collection expands each segment's support by the requested cap radius, then exact per-sample evaluation clips the Gaussian tails at four fracture widths; CPU and GPU use the same finite support."]
related = ["/rendering/terrain/material-column", "/rendering/terrain/clipmaps/generator-parity"]
depends_on = ["/foundation/core", "/foundation/math", "/world/planet-coordinates"]
used_by = ["/rendering/terrain/contracts", "/rendering/terrain/material-column"]
verify = [
  "ctest -R Orbit.TerrainImpacts"]
verified = "f3109fae"
+++
