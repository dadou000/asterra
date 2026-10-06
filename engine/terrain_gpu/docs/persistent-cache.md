+++
path = "/rendering/terrain/gpu-cache"
title = "Persistent GPU terrain cache"
kind = "concept"
status = "stable"
owner_module = "OrbitTerrainGpu"
summary = """
PersistentGpuTerrainCache keeps expensive solved/derived terrain pages (material column, drainage, erosion, scatter, river \
geometry, physical surface) resident in GPU memory, keyed only by stable physical page address, physical LOD and terrain \
authority revisions - never by camera, clipmap, viewport or slot. A hit costs nothing; a revision change makes a new key; \
InvalidateAddress drops every entry of one page. Bounded by bytes and page count with LRU eviction."""
keywords = ["gpu cache", "persistent cache", "terrain page", "lru", "eviction", "resident bytes", "cache key", "revision", "get or create", "regeneration", "physical page", "m26"]
sources = [
  "engine/terrain_gpu/include/orbit/terrain_gpu/PersistentGpuTerrainCache.hpp",
  "engine/terrain_gpu/src/PersistentGpuTerrainCache.cpp",
  "engine/terrain_gpu/tests/PersistentGpuTerrainCacheTests.cpp",
  "engine/studio_session/src/StudioTerrainPhysicalPageService.cpp",
  "engine/studio_ui/src/StudioViewportRendererBase.cpp",
  "docs/V0.0.4_M26_PERSISTENT_GPU_TERRAIN_CACHE.md",
]
symbols = ["PersistentGpuTerrainCache", "PersistentGpuTerrainCacheKey", "CachedGpuTerrainPage", "CachedTerrainProduct", "PersistentGpuTerrainCacheConfig", "PersistentGpuTerrainCacheFingerprint", "InvalidateAddress", "GetOrCreate"]
invariants = [
  "The cache key is only {PhysicalTerrainPageAddress, physicalLod, TerrainGenerationRevisions}: camera, render clipmap, viewport, frame index and GPU slot identity must never enter it, or stationary use would regenerate.",
  "A page is cacheable only if it declares solved/derived products (non-zero product mask), owns at least one real RHI buffer/texture and has non-zero resident bytes; cheap standalone procedural noise octaves are not cache products. Insert and GetOrCreate throw on a non-cacheable page.",
  "A miss generates and a hit returns the resident page (GetOrCreate, used by the tests; production code does the same with Find then Insert), so stationary use and revisiting still-resident terrain cause zero regeneration after warmup.",
  "Pages are shared_ptr: in-flight solver/renderer users keep RHI resources alive after the index evicts or invalidates a page.",
  "Budget defaults are 512 MiB and 4096 pages; eviction is deterministic least-recently-used by access serial and runs after every insert. A single page larger than the byte budget cannot stay resident (it is evicted by its own insertion).",
  "Revision changes create new keys naturally; InvalidateAddress(address) removes EVERY physical-LOD and revision entry of one page address and returns how many were removed - it is the M27 dependency-invalidation handoff.",
  "ResidentBytes counts each distinct buffer/texture once even if shared by several slots.",
  "The class has no internal synchronisation (no mutex in PersistentGpuTerrainCache): callers must serialise access.",
  "Telemetry (PersistentGpuTerrainCacheStats): hits, misses, generations, insertions, evictions, resident pages and resident GPU bytes.",
]
related = ["/rendering/terrain/invalidation", "/rendering/terrain/clipmaps"]
depends_on = ["/rendering/terrain"]
used_by = ["/rendering/terrain/invalidation"]
verify = [
  "ctest -R Orbit.TerrainGpuPersistentCache: RHI byte accounting, one generation after stationary warmup, revisit reuse, physical-LOD/revision identity, LRU behaviour, external lifetime after eviction, address invalidation, stable physical-only fingerprints.",
  "Stationary camera after warmup: hits grow, generations and misses do not.",
]
verified = "b0a0de7f"

[[diagnose]]
symptom = "terrain pages regenerate while the camera is stationary, or revisiting terrain is slow"
steps = [
  "Read the cache Stats(): after warmup generations must stop growing; misses growing while stationary means the key is unstable (something camera- or frame-dependent leaked into it) or entries are being invalidated every frame.",
  "Check for a revision that changes every frame (TerrainGenerationRevisions) or a repeated ApplyChange: every affected address is invalidated through InvalidateAddress.",
  "Check the budget: if resident pages sit at maximumPages or resident bytes at maximumResidentBytes with evictions growing, the working set exceeds the cache; a page bigger than the byte budget will never stay resident.",
  "The statistics are shown in Studio panels (SurfaceAuthoringUi, ProjectAuthoringUi); I found no dedicated RPC/MCP method for them, so read them in the UI or add one.",
]
docs = ["/rendering/terrain/invalidation"]
+++

## Products a cached page can hold

`CachedTerrainProduct` bits: MaterialColumn, Drainage, Hydraulic, Thermal, Aeolian, SedimentExchange, Coastal, Scatter,
RiverGeometry, PhysicalSurface. The final M12 physical surface has an explicit `physicalSurface` slot; generic `buffers` and
`textures` hold the rest - callers must never infer product identity from vector ordering.

## Consumers

- `StudioTerrainPhysicalPageService` (session) owns the cache (`Cache()`), the per-page status and upload revisions.
- `StudioViewportRendererBase.cpp` uses `Find` / `Insert` / `Erase` (not `GetOrCreate`, which only the tests call) to attach the
  PhysicalSurface product: it skips snapshots whose `revisionFingerprint` no longer matches the page status, builds the key from
  the snapshot's address, physical LOD and revisions, and when the cached page lacks the product it uploads the buffer, inserts an
  augmented COPY of the page, and `Erase`s the key again if `CompleteUpload` reports failure.
- Invalidation arrives from `TerrainDependencyGraph::ApplyChange` and `UnregisterPage` (`/rendering/terrain/invalidation`).
