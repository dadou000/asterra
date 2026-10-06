+++
path = "/rendering/terrain/invalidation"
title = "Dependency-driven terrain invalidation"
kind = "concept"
status = "stable"
owner_module = "OrbitTerrainDependency"
summary = """
TerrainDependencyGraph wires terrain outputs into the existing ProceduralGraph (no second scheduler). Every registered page \
owns a source -> geology -> drainage -> terrain processes -> exposed surface -> biome weights -> {surface material, scatter} \
chain. A TerrainChangeKind dirties only the products that consume it, advances only its revision domain, and is applied to \
pages inside a bounded tile neighbourhood; the affected page addresses are invalidated in the persistent GPU cache."""
keywords = ["invalidation", "dependency", "procedural graph", "terrain change", "revision", "biome edit", "rock physics", "scatter", "surface material", "spatial scope", "neighborhood", "downstream radius", "m27", "dirty"]
sources = [
  "engine/terrain_dependency/include/orbit/terrain_dependency/TerrainDependencyGraph.hpp",
  "engine/terrain_dependency/src/TerrainDependencyGraph.cpp",
  "engine/terrain_dependency/tests/TerrainDependencyGraphTests.cpp",
  "engine/terrain/include/orbit/terrain/TerrainContracts.hpp",
]
symbols = ["TerrainDependencyGraph", "TerrainChangeKind", "TerrainInvalidationRequest", "TerrainSpatialInvalidationScope", "TerrainInvalidationResult", "TerrainDependencyProduct", "TerrainGenerationRevisions", "ProductsForChange", "ApplyChange", "UnregisterPage"]
invariants = [
  "Terrain outputs are scheduled by the existing ProceduralGraph; do not introduce a second scheduler or build queue for terrain products.",
  "Per page the product chain is Geology -> Drainage -> TerrainProcesses -> ExposedSurface -> BiomeWeights -> {SurfaceMaterial, Scatter}; Drainage, TerrainProcesses and Scatter run on the GPU backend, the others on the CPU.",
  "Which products a change dirties (DirtyProductsFor): RockPhysics and TerrainAuthoring -> all seven; Climate -> all but Geology; Water and ProcessSettings -> TerrainProcesses and everything after it; BiomePlacement -> BiomeWeights, SurfaceMaterial, Scatter; BiomeSurfaceMaterial -> SurfaceMaterial only; BiomeScatter -> Scatter only.",
  "Biome edits have three separate source branches (placement/rules, surface material, scatter) and none is an ancestor of erosion: a tree-density edit never reruns terrain processes and a moss material edit never touches scatter.",
  "Each affected page advances only the revision domain of the change (geology, authoring, climate, water, processes) - all three biome kinds advance the single `biome` domain.",
  "ApplyChange removes ALL persistent-GPU-cache entries of every affected page address (InvalidateAddress), whatever the kind: cache invalidation is address-granular, not product-granular.",
  "Bounded scopes work at the physical page tile level of scope.center: only pages on the SAME level within radiusTiles + downstreamRadiusTiles (clamped to 64) are affected. Edits spanning levels must be projected by the caller or expressed as global.",
  "An invalid scope throws std::invalid_argument; the default scope is global.",
  "UnregisterPage never blocks: it returns false while any node of the page has in-flight work, and on success also invalidates the page address in the cache.",
  "Distant pages outside the bounded neighbourhood keep clean committed products.",
]
related = ["/rendering/terrain/gpu-cache"]
depends_on = ["/rendering/terrain", "/rendering/terrain/gpu-cache"]
verify = [
  "ctest -R Orbit.TerrainDependency: forest tree-density rebuilds scatter without rerunning terrain processes; moss change rebuilds surface material only; bedrock change rebuilds geology and erosion descendants; one-tile downstream reach leaves distant pages clean; biome scatter edits advance only the biome revision; affected cache entries are invalidated.",
  "TerrainInvalidationResult after an edit: affectedPages, cacheEntriesRemoved and dirtyProducts match the table in the invariants.",
]
verified = "b0a0de7f"

[[diagnose]]
symptom = "a small edit rebuilds too much terrain (erosion reruns after a biome edit) or pages far from the edit rebuild"
steps = [
  "Check the TerrainChangeKind the edit was submitted with: only RockPhysics/TerrainAuthoring dirty geology and everything below; biome kinds must be BiomePlacement, BiomeSurfaceMaterial or BiomeScatter.",
  "Read TerrainInvalidationResult.dirtyProducts and affectedPages; for a local edit scope.global must be false with a sensible radiusTiles/downstreamRadiusTiles.",
  "Remember the radius applies on scope.center's tile level only; pages at other levels are untouched by a bounded scope, and a global scope affects every registered page of the planet.",
]
docs = ["/rendering/terrain/gpu-cache"]

[[diagnose]]
symptom = "after a terrain edit the rendering still shows old terrain, or an edit seems to have no effect on some pages"
steps = [
  "Confirm the page is registered (ContainsPage) and inside the bounded neighbourhood on the same tile level as the scope centre.",
  "Check the page's revisions advanced (Revisions(address)); the cache key includes them, so a stale view means the old key is still being requested.",
  "A scatter-only or material-only edit leaves other products clean on purpose; if a downstream product did not update, check the dependency chain for that product rather than invalidating globally.",
]
docs = ["/rendering/terrain/gpu-cache"]
+++

## Source nodes and dependency chain

```text
rock physics + authoring      -> geology
climate + authoring           -> drainage
climate + water + process     -> terrain processes
                              -> exposed surface
climate + biome placement     -> biome weights
                biome weights -> biome surface material (+ surface-material source)
                biome weights -> biome scatter           (+ scatter source)
```

The graph's source kinds are RockPhysics, Authoring, Climate, Water, ProcessSettings, BiomePlacement, BiomeSurface and
BiomeScatter; `TerrainChangeKind` maps one-to-one onto them.

## Build seams

`RequestBuild(address, product)` is the non-blocking seam used by editor/runtime schedulers and `Poll()` collects results;
`BuildBlocking` is the blocking variant. Stale generations are rejected by the underlying `ProceduralGraph`, and the
build function receives the revisions current at execution time.

## Observation (not a bug report)

Because cache invalidation is per address, a `BiomeScatter` edit leaves the graph's drainage and process products clean but
still releases every cached GPU page of the affected addresses. Whether that costs a regeneration depends on how consumers
rebuild from clean graph products; measure with the cache stats before treating it as a problem
(`/rendering/terrain/gpu-cache`).
