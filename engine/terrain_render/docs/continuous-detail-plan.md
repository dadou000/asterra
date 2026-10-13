+++
path = "/rendering/terrain/clipmaps/continuous-detail-plan"
title = "Continuous terrain features and adaptive near geometry: implementation plan"
kind = "concept"
status = "planned"
owner_module = "OrbitTerrainRender"
summary = "Extend existing indexed curve/feature authority with local-coordinate relief derivatives and error estimates; then prototype bounded adaptive near meshes while keeping toroidal clipmaps for coverage. This is a plan, not implemented vector displacement or adaptive tessellation."
keywords = ["vectors", "displacement", "adaptive mesh", "curves", "spline", "residual", "precision", "terrain detail"]
sources = [
  "engine/terrain/include/orbit/terrain/AnalyticTerrainSource.hpp",
  "engine/terrain/include/orbit/terrain/BakedRivers.hpp",
  "engine/terrain_impacts/include/orbit/terrain_impacts/ImpactField.hpp",
  "engine/terrain_erosion/include/orbit/terrain_erosion/RiverCarving.hpp",
  "engine/surface_authoring/include/orbit/surface_authoring/TerrainConstraints.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuGeologyCompiler.hpp",
  "engine/terrain_dependency/include/orbit/terrain_dependency/TerrainDependencyGraph.hpp",
  "engine/terrain_render/src/ClipmapVertexShader.hpp"]
related = ["/rendering/terrain/clipmaps/close-range-review", "/rendering/terrain/contracts", "/world/terrain-constraints", "/rendering/terrain/invalidation"]
invariants = [
  "Semantic terrain and authored edits remain authority; mesh, collision and sampled products are derived caches.",
  "New fine detail must preserve local-coordinate precision and differentiate the final composed displacement.",
  "Near refinement must agree at shared edges and the clipmap boundary, with explicit triangle/work budgets and stale-result rejection."]
verified = "8dc17324af29e897a3eb204ad9d93e3319b832c4"
+++

## What already exists

`AnalyticTerrainSource` is continuous and footprint-filtered. Impact/fracture
features have immutable spatial indexes and conservative query batches.
`BakedRiverNetwork` and `RiverCarvingField` represent centerlines and channel
profiles with local segment buckets. `TerrainConstraintSet` persists point,
brush, spline and polygon edits. `GpuGeologyCompiler` and
`TerrainDependencyGraph` already own bounded compilation and invalidation.

The renderer still consumes scalar elevation on fixed cells (six vertices per
cell). Adaptive ring selection is not adaptive triangle refinement. Increasing
uniform grid resolution cannot resolve an entire planet at centimetre scale.

## Proposed increments

1. Repair and validate final-surface normal/height ownership first.
2. Extend existing authored spline compilation with immutable finite-support
   profile segments and conservative spatial queries. Reuse current river or
   fracture shapes for the prototype. Return relief, genuine derivatives,
   influence bounds and geometric error estimates at arbitrary off-grid points.
   Authored gradient guidance is separate data, not the height derivative.
3. Preserve double canonical anchors plus local metre offsets through CPU/GPU
   evaluation. Converting first to an absolute float planetary direction loses
   centimetre detail; changing scalar height into a float3 displacement texture
   does not remove grid density or precision limitations.
4. Define the residual: coarse/baked surface + physical changes + fine feature
   contribution **minus its already baked contribution**. Use one revision and
   surface contract for rendering, water, collision and material queries.
5. Add a bounded adaptive near patch inside the existing terrain renderer.
   Refine by projected geometric deviation/curvature with a hard triangle and
   evaluation budget. Match edge subdivisions and fixed handoff vertices; fade
   or sinking alone cannot make two different boundaries watertight.

Start with radial profiles. True lateral displacement also needs Jacobian
normals, conservative bounds, inverse surface queries and fold/intersection
handling. Caves and multivalued surfaces require local topology beyond the
current one-height-per-direction contract. Camera-floor protection is not a
terrain collision implementation.

## Acceptance and cost

Verify off-grid CPU/GPU relief/derivative agreement, cube seams, edit invalidation,
page identity, collision error and watertight LOD transitions. Compare at identical
poses against current clipmaps: GPU time, resident memory, generated triangles,
feature evaluations and cache misses. Test shorelines, steep cliffs, fast motion,
floating-origin changes and orbit-to-ground travel. No performance numbers are
claimed for this proposed path.

Authoritative records stay outside replaceable implementation generations;
central hot iteration validates replacement products before activation. Reuse
existing resource retirement and asynchronous scheduling rather than adding a
new watcher or global controller.

## Research rationale

[GPU-based geometry clipmaps](https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry)
demonstrate bounded toroidal residency and deriving normals from the level's
elevation data. Retaining that coverage structure is the council's inference
for Orbit, not a requirement to preserve uniform near topology.

[Generic Adaptive Mesh Refinement](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-5-generic-adaptive-mesh-refinement)
demonstrates shared refinement patterns and separating tessellation from
displacement. Its historical timings do not predict Orbit performance; use the
edge-consistency principles and benchmark the actual Vulkan implementation.
