# Asterra terrain performance architecture

Status: implementation plan for `terraintesting/0.0.5` and the path toward the production terrain renderer.

The objective is not to make `ChunkBuilder` fast enough to synthesize a planet while the player moves. The objective is to remove terrain synthesis and mesh construction from the normal runtime path.

## 1. Targets

Asterra must support all of these at the same time:

- seamless surface-to-orbit traversal on a spherical planet;
- approximately 0.75 m ground geometry near the player;
- high-speed ground vehicles without terrain-generation stalls;
- aircraft crossing large distances quickly;
- persistent terrain editing;
- a low-end graphics path that remains viable on GTX 1050-class hardware;
- deterministic world generation and multiplayer-compatible terrain deltas.

### Runtime budgets

These are design budgets, not promises from the current prototype:

| subsystem | target |
| --- | ---: |
| terrain work on main thread | < 1 ms/frame typical, < 2 ms worst case |
| runtime procedural terrain synthesis in shipped worlds | 0 samples/frame |
| local `ChunkBuilder` jobs after ground clipmap is resident | 0 |
| ground render submissions | 1-2 preferred, <= 6 acceptable during migration |
| ground CPU mesh rebuilds while travelling | 0 |
| terrain I/O | asynchronous and predictive |
| terrain worker saturation | never allowed to starve render/physics |
| collision coverage | guaranteed ahead of simulated bodies |

The runtime may temporarily display a coarser valid representation when fine data has not arrived. It must not stall the game thread to manufacture missing visual detail.

## 2. Final representation hierarchy

The planet should not use one render system at every scale.

### Tier A - orbit / upper atmosphere

Purpose: planet silhouette and satellite-scale appearance.

- very coarse sphere/cube-sphere geometry;
- baked height/displacement at orbital scale;
- very high resolution tiled/virtual surface atlas;
- baked macro normal/roughness/water/snow/vegetation controls;
- no local ground chunks;
- no `ChunkBuilder` refinement to sub-kilometre scales.

The orbital texture carries most apparent detail. Geometry only needs enough resolution for the silhouette and large mountain ranges.

### Tier B - regional terrain

Purpose: low atmosphere and long-distance landscape.

- baked height pyramid, roughly tens to hundreds of metres per sample;
- coarse global quadtree only for visibility/culling and spherical coverage;
- no expensive procedural synthesis per vertex;
- material and normal data streamed from the same authoritative bake.

### Tier C - ground geometry clipmap

Purpose: walking, vehicles, structures and close flight.

Current target levels:

| level | approximate spacing |
| ---: | ---: |
| L0 | 0.75 m |
| L1 | 1.5 m |
| L2 | 3 m |
| L3 | 6 m |
| L4 | 12 m |
| L5 | 24 m |
| backing | 48 m |

The meshes are fixed concentric rings. Moving across the world changes data, not topology.

Requirements:

- ring origin snaps to the backing lattice;
- fine levels morph to the exact next coarser representation before topology boundaries;
- outer clipmap converges to the global 48 m representation before handoff;
- no visible square rings;
- no CPU mesh rebuilds during travel;
- velocity-biased prefetch so high-speed vehicles load terrain ahead, not symmetrically behind.

### Tier D - micro surface detail

Anything much smaller than the 0.75 m geometry lattice should normally be represented by:

- PBR material textures;
- shader normals;
- decals;
- vegetation/rocks/meshes;
- optional bounded tessellation/microdisplacement only where hardware and the engine path permit it.

Do not increase CPU heightfield density merely to represent centimetre-scale visual roughness.

## 3. Authoritative offline world compiler

The production world is compiled, not synthesized during gameplay.

Pipeline:

```text
seed/config
   -> geology / climate / hydrology / erosion
   -> authoritative high resolution surface
   -> filtered multiresolution pyramid
   -> height residual tiles
   -> material/control tiles
   -> orbital atlas + macro normal pyramid
   -> collision source pyramid
   -> manifest/index
   -> packaged world data
```

The compiler is allowed to take minutes or hours. Runtime is not.

### Data products

Each tile should eventually contain or reference:

- quantized height/residual data;
- min/max elevation;
- geometric error bound;
- surface/material weights;
- water coverage and relevant water level data;
- optional macro normal data;
- optional vegetation/biome density controls;
- checksums/version/world signature.

Normals should generally be reconstructed from height on the GPU rather than stored at ground resolution.

## 4. Hierarchical residual storage

Do not store seven unrelated full-resolution heightfields when the finer data can be represented as a correction to the parent.

Conceptually:

```text
H48
 + residual24
 + residual12
 + residual6
 + residual3
 + residual1.5
 + residual0.75
```

Benefits:

- exact parent/child consistency;
- exact geomorph targets;
- much better compression;
- missing fine data naturally falls back to a valid coarse surface;
- fewer visible LOD pops.

The current `.ghz` full-float tile cache is a migration format. Production should move to quantized residual tile packs after the runtime streamer is proven.

## 5. Runtime height tile service

All consumers must share one immutable base-height cache.

Consumers include:

- ground geometry clipmap;
- collision streamer;
- terrain queries that need physical ground;
- future road/building placement systems.

Runtime layers:

```text
packaged world tiles (res:// or external world package)
        -> async disk read/decompression
        -> RAM LRU cache
        -> GPU height tile/page cache
        -> render / collision consumers

player edits remain a separate sparse delta layer
```

### Non-blocking rule

Visual rendering must never synchronously generate a missing tile.

If a requested fine tile is absent:

1. enqueue it;
2. render the nearest resident coarser height;
3. upgrade when the tile arrives;
4. morph rather than pop where practical.

During development, a cache miss may be generated once in a low-priority background worker and persisted. Shipping worlds should be precompiled so this generation path is normally unused.

## 6. Predictive streaming

The cache target is not just the camera position.

Use position, velocity and simulation interest points:

```text
predicted_position = position + velocity * lookahead_seconds
```

Suggested behavior:

- walking: mostly symmetric fine coverage;
- car: bias L0-L2 strongly ahead;
- aircraft: bias regional tiles far ahead and reduce fine-ring urgency;
- teleport: immediately provide coarse coverage, cancel obsolete fine requests, refill around the destination.

Requests require explicit priority classes:

1. collision directly under/ahead of active simulated bodies;
2. visible missing coverage;
3. fine visual upgrades;
4. speculative prefetch;
5. cosmetic/orbital refinement.

No FIFO queues for spatial streaming.

## 7. GPU geometry path

The current fixed `ArrayMesh` rings are an intermediate implementation. They are already cheap compared with the old `ChunkBuilder`, but the final renderer can reduce state and bandwidth further.

### Vertex-ID procedural ring renderer

Inspired by the Vercidium terrain renderer:

- derive grid coordinates from `VERTEX_ID`;
- use `INSTANCE_ID` or draw ranges to select clipmap level;
- sample the GPU height pyramid directly;
- reconstruct world position in the vertex shader;
- reconstruct normals from neighbouring height samples;
- avoid per-terrain position/normal/UV streams;
- use triangle strips or compact static topology;
- batch rings into one or very few submissions.

Preferred final implementation is through Godot's lower-level `RenderingDevice`/RenderingServer path or the engine fork, rather than forcing unsupported zero-vertex behavior through `ArrayMesh`.

This is primarily a GPU/draw-overhead optimization. It comes after removing runtime CPU terrain synthesis.

## 8. GPU tile/page cache

The CPU should not rebuild a complete `Texture2DArray` every time one terrain strip changes.

Production direction:

- persistent GPU height page cache/atlas;
- upload only newly exposed rows/pages;
- toroidal or page-table addressing;
- retain tiles across nearby clipmap movements;
- reuse the same GPU data for normal reconstruction and material lookup where possible.

The current clipmap image scrolling is a functional intermediate stage.

## 9. Materials and orbital appearance

Geometry LOD and apparent surface detail are separate problems.

### Orbit

Use a high-resolution tiled atlas/virtual texture. Target an effective resolution in the 64k-128k-class range for the whole planet, streamed by mip/tile rather than loaded as one texture.

Bake at least:

- albedo;
- macro normal;
- roughness;
- water mask;
- snow/ice mask;
- vegetation/land-cover tint;
- optional geological tint.

### Ground

Use material clipmaps/virtual textures with stable world coordinates. Material detail must not phase-shift across floating-origin rebases.

Do not bake centimetre detail into height geometry.

## 10. Collision architecture

Collision must consume the same authoritative height tiles as rendering but has different spatial priorities.

Target collision rings/bubbles:

- near player/vehicle: 0.5-0.75 m where required;
- medium range: 1.5-3 m;
- farther safety coverage: coarse terrain;
- velocity lookahead ahead of vehicles;
- multiple simulation interest points for multiplayer, AI, dropped bodies and remote vehicles.

Collision cannot be camera-only in the final architecture.

The center/ahead tile must always be first. Collision generation may not compete with visual terrain synthesis because the immutable height data should already be cached.

## 11. Terrain editing

Keep the base world immutable:

```text
final_height = baked_base_height + sparse_player_delta
```

Advantages:

- base tiles remain shareable and cacheable;
- multiplayer only replicates edit deltas;
- saves remain small;
- rebaking/repackaging the base world is separate from player state.

Edited regions invalidate only the affected visual/collision pages, never the entire base terrain database.

## 12. Scheduler/backpressure rules

Even background work needs hard limits.

- no more than a small fixed number of heavy terrain workers;
- cap pending work;
- cancel stale requests before they start;
- preserve revision/dirty state through cancellation;
- prioritize by visibility, distance, motion and physics importance;
- never allow a low-FPS feedback loop to increase terrain work per frame;
- main-thread installation has an explicit microsecond budget.

## 13. Instrumentation

The HUD/profiler should eventually expose:

- ground cache RAM hits;
- disk hits;
- cache misses/builds;
- async tile queue/in-flight count;
- bytes read/decompressed/uploaded per second;
- GPU tile residency;
- clipmap fallback level currently visible;
- collision queue and coverage radius;
- global quadtree queue/in-flight count;
- terrain CPU milliseconds and GPU milliseconds separately.

Performance work without these counters becomes guesswork.

## 14. Migration phases

### Phase 0 - current prototype

Already present on `terraintesting/0.0.5`:

- global quadtree capped around depth 10;
- six fixed ground geometry rings;
- 0.75 -> 48 m height pyramid;
- global terrain cutout/handoff;
- persistent `.ghz` ground tile cache;
- visual and collision terrain share the height store.

### Phase 1 - non-blocking runtime tile service

**Implement first.**

- async disk/load/generate queue inside `GroundHeightStore`;
- non-blocking visual sampling;
- coarse fallback on a fine miss;
- tile-ready notifications;
- debounce clipmap refreshes as pages arrive;
- keep one low-priority development baker so first visits do not saturate the CPU.

Success criterion: entering uncached terrain may initially look coarser, but must not collapse frame rate merely to synthesize visual ground.

### Phase 2 - predictive prefetch + collision integration

- velocity-aware tile priority;
- prefetch future ground window;
- collision asks for tiles before building bodies;
- no duplicate procedural generation between collision and rendering;
- multi-interest-point collision bubbles.

### Phase 3 - offline regional/world compiler

- command-line/headless compiler;
- bake selected regions and important gameplay zones at full resolution;
- package tiles under the same runtime namespace/layout;
- produce manifest and statistics;
- later expand to full production world packages.

### Phase 4 - residual/quantized tile format

- parent-relative residuals;
- 16-bit or adaptive quantization with per-tile scale/offset;
- gutters/checksums/error metadata;
- packed tile archives to avoid millions of tiny filesystem files.

### Phase 5 - GPU page cache

- persistent height atlas/page table;
- incremental page uploads;
- no recreation of entire texture arrays for one strip;
- GPU normal reconstruction from resident heights.

### Phase 6 - procedural vertex-ID renderer

- prototype `VERTEX_ID` coordinate reconstruction;
- triangle-strip/static procedural topology;
- instance/batch all rings;
- migrate to RenderingDevice/engine-level render path if it materially reduces CPU/GPU overhead;
- retain exact hierarchical morphing rather than relying only on terrain sinking.

### Phase 7 - orbital virtual surface

- compiler-generated high-resolution surface pyramid;
- tiled streaming from space through atmosphere;
- baked macro normals and material controls;
- transition into regional/ground material representation without an obvious texture swap.

### Phase 8 - remove legacy local ChunkBuilder path

Once the baked regional + ground system has proven parity:

- `ChunkBuilder` remains only where genuinely useful for coarse/global fallback, or is replaced by static procedural grids there too;
- delete unreachable depth-11..16 visual build paths;
- delete duplicated local terrain synthesis code;
- make runtime procedural generation a developer/compiler feature rather than gameplay infrastructure.

## 15. Definition of done

The production terrain system is finished when a warm/precompiled world can move from ground to orbit and back while:

- local terrain performs no procedural synthesis;
- ground topology is never rebuilt because of movement;
- fine data streams ahead of fast vehicles;
- missing data degrades to coarse valid terrain rather than stalling;
- LOD rings are not visually detectable under normal motion/lighting;
- edits remain stable and physics matches the edited visual terrain;
- orbit appearance is independent of coarse orbital mesh density;
- terrain CPU load remains bounded and predictable regardless of travel speed.
