# Asterra 0.0.5 terrain runtime

The production terrain entry point is `spherical_geometry_clipmap_authoritative.gd`.
Older terrain/page/quadtree implementations remain in the repository only as compatibility or experiment history and must not be used as new runtime dependencies.

## Authoritative runtime stack

1. `Planet` owns resident global macro terrain/context maps.
2. `GroundGeometryClipmap` owns spherical concentric visual topology and screen-space LOD.
3. `GPUTerrainClipmapCache` caches immutable pristine procedural heights only.
4. `Deltas` is the persistent authoritative mutable terrain edit lattice.
5. `TerrainEditDeltaGPU` mirrors the nearby edit lattice into an R32F GPU texture; the final terrain shader adds this after pristine procedural displacement.
6. `TerrainHeightQuery` is the pooled asynchronous precise terrain query service. It evaluates pristine terrain on the GPU and applies `Deltas` to returned gameplay heights/normals.
7. `TerrainContactSampler` is the public gameplay façade for height, normal, altitude and batched surface requests.
8. Vehicles use height-field support probes rather than planet-scale CPU ConcavePolygon collision tiles.

The invariant is:

`authoritative terrain height = pristine GPU terrain + mutable Deltas`

Visual rendering, walking, aiming/edit targeting and rigid-body terrain support must preserve that invariant.

## Projection

The spherical clipmap uses a gnomonic near mapping and smoothly transitions to geodesic angular distance from 70 km to 110 km from an anchor. Renderer and cache compute must keep the same formula. Do not reintroduce a hard distance switch.

## Cache ownership

The active 512x512x15 terrain cache is always resident. A second staging cache is allocated only while a predicted anchor handoff is being warmed and is released after commit/cancel. Analytic shader fallback remains authoritative for cache misses.

## Legacy code

Files implementing old quadtree chunks, page atlases, CPU terrain detail collision streaming, and earlier geometry-clipmap generations are not part of the current production terrain authority. They may remain until dependent debug/compatibility code is removed, but new systems should target the façade/services above instead of importing them directly.
