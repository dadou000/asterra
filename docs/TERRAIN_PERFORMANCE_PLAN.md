# Asterra terrain architecture

Status: active architecture for `terraintesting/0.0.5`.

The terrain renderer no longer streams visual height pages. The planet has one immutable, resident low-frequency elevation field; all finer untouched terrain is synthesized deterministically on the GPU at the clipmap vertices. Runtime storage is reserved for player edits and local physics structures, not for visual terrain coverage.

## 1. Runtime goals

Asterra must support seamless surface-to-orbit travel, approximately 0.75 m near-ground geometry, fast vehicles and aircraft, persistent editing, spherical terrain and bounded CPU work.

The hard runtime rule is now:

```text
world adopted
    -> global height field resident
    -> no visual terrain I/O while travelling
```

Normal movement must perform:

- zero terrain page reads;
- zero terrain page decompression;
- zero visual terrain cache misses;
- zero CPU visual terrain synthesis;
- zero terrain mesh rebuilding because the camera moved.

## 2. Whole-planet base height

The visual base is one six-face cube-sphere elevation texture:

```text
6 cube faces
2048 x 2048 samples per face
32-bit float elevation
seam-safe gutter
complete mip chain
```

For a 1000 km-radius planet, 2048 samples across a quarter circumference are about 767 m apart before interpolation. This map intentionally contains only the low-frequency shape of the world: continents, basins, mountain masses, major valleys and coast-scale relief.

The height field is reconstructed from the authoritative 192-cell generated macro grid using the seam-safe C2 cubic B-spline, then supersampled in native image code. The shader reconstructs it cubically again rather than exposing piecewise-linear heightmap cells.

### Persistent format

The six RF faces and all mip levels are stored in one `.aghm` package:

```text
user://global_heightmaps/<world signature>.aghm
```

The image payload is compressed losslessly with ZSTD. The package is keyed by `GenConfig.cache_key()`, resolution and format version, so an incompatible world automatically gets a different file.

Runtime behavior:

```text
cache exists
    -> read one compressed file
    -> decompress six faces once
    -> create Texture2DArray once
    -> terrain fully resident

cache missing (development fallback)
    -> build global height field once
    -> build mip chain
    -> write compressed package
    -> create Texture2DArray
```

Production/world-compiler builds should package this file ahead of time so the development fallback is not part of normal gameplay startup.

## 3. GPU detail from L0 to L14

The visual terrain function is continuous and deterministic in planet/world space:

```text
H(level, direction)
    = filtered_global_height(direction, level_spacing)
    + procedural_detail(direction, level_spacing)
    + edit_delta(direction)
```

The global height map is sampled at an appropriate mip for the current vertex spacing. Procedural detail uses fixed physical wavelengths and each frequency band fades out before the current LOD can undersample it.

Conceptually:

```text
L14  global + only very broad procedural bands
 ...
L4   global + regional + local bands
L3   global + regional + local + finer bands
L2   same world function with more bandwidth
L1   same world function with more bandwidth
L0   full geometry-frequency spectrum
```

The detail function is evaluated directly by the terrain vertex shader. There is no L0-L14 height-page database.

## 4. Spherical concentric geometry clipmap

The active renderer is a camera-centred spherical geometry clipmap.

Near-ground target spacing begins around 0.75 m and doubles per level. The 400-cell concentric topology provides the current 4K / roughly 16-pixels-per-terrain-vertex target.

Each logical level is either the centre disc or a circular annulus. The annuli are partitioned into angular sectors for view culling, but explicit logical UV coordinates ensure sector compaction cannot change terrain vertex addressing.

Projection is spherical. Small arcs may use the cheap normalized tangent approximation; large arcs use the exponential/geodesic map. The projection decision is based on physical arc distance so overlapping LODs cannot disagree merely because their level numbers differ.

## 5. LOD morphing and sinking

Two mechanisms cooperate at every handoff.

### Spectral / height morph

The outside of a fine level morphs toward the same terrain function evaluated with its parent spacing. This smoothly removes frequencies the parent cannot represent.

### Geometric sinking

The inside edge of the coarser annulus extends underneath the finer surface and sinks radially into the planet:

```text
position = direction * (planet_radius + height - sink)
```

This guarantees overlap and avoids depending on exact T-junction stitching. Sinking is a topology/coverage mechanism; the fine-to-parent morph keeps the two surfaces visually close before the hidden overlap.

Debug tools can freeze the clipmap, cut half of it away, exaggerate sink depth and label active rings as L0, L1, L2, etc.

## 6. Normals

Terrain lighting must not derive normals from individual rasterized triangle planes. The clipmap lattice moves and reanchors, so flat per-triangle normals would create a camera-dependent checker pattern even if the height function itself were perfectly stable.

The current path reconstructs a smooth planet-space normal from the continuous macro surface with a broader sample footprint than the displacement. Future terrain process composition should preferably provide analytic or cheaply sampled gradients for its procedural bands and add those gradients to the smooth base normal.

## 7. CPU physics without terrain streaming

Physics does not read the GPU back and it does not load height pages from disk.

`GroundHeightStore` is now an immediate resident sampler:

```text
CPU query
    -> Planet.macro_height()       # compact resident C2 field
    -> deterministic detail bands # same seed/spectrum as GPU
    -> sparse edit delta
```

The old request/residency methods remain temporarily as zero-cost compatibility shims for `TerrainCollisionStreamer`. A source query is always resident once `Planet` is ready.

`TerrainCollisionStreamer` still builds local collision geometry asynchronously around active simulation interest points, because Godot physics needs CPU collision shapes. What disappeared is the page-loading prerequisite before those builds.

Future work should replace triangle-soup collision tiles with heightfield-oriented shapes where practical and support multiple physics bubbles for remote vehicles, trains and multiplayer actors.

## 8. Editing

The immutable world remains separate from mutable gameplay state:

```text
final_height
    = immutable global/procedural terrain
    + sparse player delta
```

Only edited regions consume persistent runtime terrain storage. This keeps saves and multiplayer replication proportional to actual modification rather than planet area.

## 9. What is obsolete

The following are no longer part of the active visual architecture:

- `.ghz` visual height pages;
- visual `GroundHeightStore` page queues;
- RAM terrain-page LRU for rendering;
- GPU height page atlas;
- GPU height page table;
- page birth/fade metadata;
- camera terrain-page prefetch;
- runtime visual page baking;
- CPU `ChunkBuilder` visual meshes.

Some old source files and tiny disabled autoload shims still exist during migration because older debug/base scripts contain those API names. They allocate no height atlas and perform no terrain I/O. They should be deleted once the inheritance chain is flattened around the final global renderer.

## 10. Materials and orbital appearance

Geometry height and apparent surface detail are independent systems.

Ground materials should continue toward stable world-space material clipmaps / virtual textures. Centimetre-to-metre visual roughness belongs primarily in material normals, displacement techniques, rocks and vegetation rather than by increasing the base clipmap lattice indefinitely.

Orbital albedo/material data can eventually use a much larger tiled virtual surface. That does not require height pages: the global elevation texture is already sufficient for large-scale displacement and silhouette, while satellite-scale colour/normal/roughness may stream independently.

## 11. World compiler target

The production compiler should emit the global height package along with the other immutable world products:

```text
seed/config
    -> geology / hydrology / erosion / climate
    -> smoothed C2 macro surface
    -> 2048 x 2048 x 6 global elevation
    -> prefiltered mip chain
    -> ZSTD .aghm package
    -> global material/orbit products
    -> manifest
```

The runtime development fallback that creates `.aghm` after a cache miss exists only to keep iteration convenient. Shipping worlds should never need to synthesize this resource after installation.

## 12. Performance model

Steady-state terrain cost is intentionally simple:

### CPU

- update clipmap centre/tangent basis;
- determine active LOD count and visible sectors;
- maintain local physics bubbles;
- evaluate occasional CPU ground queries;
- maintain sparse edit deltas.

There is no CPU terrain coverage scheduler.

### GPU

- static concentric topology;
- cubic global height lookup;
- band-limited procedural detail per active vertex;
- smooth normal reconstruction;
- terrain material/lighting.

Memory usage is fixed by the global texture and static topology instead of changing according to where the player has travelled.

## 13. Instrumentation

Useful terrain debug counters are now:

- global height resident yes/no;
- global face resolution;
- global package cache hit/miss;
- compressed and uncompressed global height size;
- one-time load/build duration;
- active clipmap LODs;
- visible sectors / draw batches;
- collision build queue/in-flight count;
- edited tile count;
- terrain CPU and GPU frame time.

Page-residency, page-table, page-upload and terrain-prefetch counters are obsolete and should disappear from the HUD.

## 14. Definition of done

The terrain architecture is complete when a precompiled world can move from ground to orbit and back while:

- the base height resource is loaded once;
- travel triggers no visual terrain file I/O;
- travel triggers no CPU visual terrain generation;
- topology is never rebuilt because of movement;
- all LODs evaluate one continuous world-space terrain function;
- LOD transitions are not visible under normal lighting/motion;
- physics tracks the same deterministic surface locally without GPU readback;
- edits remain stable and sparse;
- terrain CPU load is bounded by simulation interest points, not travel speed or explored area.
