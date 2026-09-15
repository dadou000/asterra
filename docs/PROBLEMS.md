# Orbit problem tracker

Running list of known issues found while testing Orbit, with status. Keep entries
short: what's wrong, how it was found, and (once fixed) what changed. Add new
problems at the top of their status section.

Status legend:
- ✅ **Fixed** — verified resolved, with the fix commit/change noted
- ⚠️ **Open** — confirmed present, not yet fixed
- 🔍 **Unverified** — observed once, root cause not confirmed

---

## Open

### 🔍 Terrain changes shape on source refresh / rebase
**Correction implemented on 2026-09-13; awaiting visual confirmation.** Source
revision changes called `ClipmapTracker::Reset`, relocating every grid around
the current observer. A refresh therefore changed the sampling phase even for
distant levels whose spacing and underlying field were unchanged.

Source changes now call `InvalidateSamples`: the next candidate refreshes every
sample while retaining each grid's center and orientation. Coverage tier changes
also retain grids whose sample spacing exists in both tiers. The regression
checks eight repeated invalidations across 12 displaced grids, full refresh
coverage, consumption of the invalidation, and shared grids across an LOD change.

F3 now shows `REBASE <count> <reason> <levels> LVL <age>S`, with `NOW` for two
seconds after a committed rebuild. Reasons are `SOURCE`, `LOD`, and `MOVE`.
This counts full-grid data rebuilds, not just coordinate-origin changes; initial
population and discarded batches are excluded. `STATS` exposes the same fields.
Actual newly arrived hydrology and changing detail levels can still change
terrain content; this correction removes the unintended grid relocation.

### 🔍 Standing water rings, changing lake levels and detached shorelines
**Implemented on 2026-09-13; awaiting visual confirmation.** The orbital
screenshot has `LK 0`: its rings come from the independent ocean polar mesh
intersecting the terrain's differently tessellated sphere. Inland water also
used half-cell quads, discarded shallow cells, switched between independently
solved coarse/fine lake levels, and could truncate at 8,192 cells.

**Change:** the sandbox now renders land, ocean and lakes in the same terrain
surface pass. Bed elevation and standing-water depth are cached and morphed
together; the GPU uses their sum. Lakes use the coarse hydrology cache as their
level authority, retain shallow fringes/overlap, and sample a continuous bank
and basin bed. Separate ocean and lake overlays are disabled in the sandbox.
Details and limitations: [standing water rendering](STANDING_WATER_RENDERING.md).
Automated shoreline, cache/morph, lake-authority and shader/runtime checks pass;
this status does not claim that the user's view has been checked.

### 🔍 Soft brightness band on terrain, ~19–56 km altitude
A faint, non-hard-edged brightness/color discontinuity visible on the terrain
surface specifically in the 19–56 km altitude range (seen in two separate
`SLEW` captures at the same spot, both altitudes inside that band, not seen
below or above it). Could be a clipmap morph band overlap, or an
unrelated lighting artifact — not isolated. Not seen during the ground-level
(300 m) or mildly-high (4 km) clipmap movement test, which stayed clean.

---

## Fixed

### ✅ Spherical clipmap phase drift and seam sinking
**Correctness fix implemented on 2026-09-15; awaiting visual confirmation.**
Toroidal strip reuse treated a recentered spherical tangent frame like a flat
translation. Retained samples therefore kept height/biome/water values from
old world directions while the renderer reconstructed their geometry from the
new frame. The error is tiny on fine rings but grows rapidly on coarse rings
and can look like terrain swimming, phase jumps, or incorrect sinking.

**Change:** any spherical clipmap frame recenter now forces a full regeneration
of that level until Orbit has a stable spherical integer lattice. Coarse-ring
holes are centered on the actual finer-level frame instead of assuming both
independently snapped levels share the same center. Hole cells are rejected via
clip distance rather than `(0,0,0,0)` homogeneous vertices. The innermost
coarse overlap cell is smoothly sunk under the fine patch (5% of spacing,
capped at 8 m) to suppress residual z-fighting/raster cracks without modifying
authoritative terrain. TerrainViewTests now explicitly proves that flat-style
retained-sample translation drifts on the sphere and requires a full refresh.

### ✅ Terrain clipmap garbles during movement, cleared by rebasing
**Confirmed fixed by the user on 2026-09-13.** Reused samples contained XY morph
targets relative to an old ring center and heights/biomes blended for the old
transition band. As the toroidal origin moved, these stale values stretched
triangles and distorted heights. Full refreshes regenerated them correctly.

**Fix:** `RefreshTerrainMorphRegions` refreshes the current and previous morph
bands when either the level or its coarser parent moves, unions these with
exposed strips, and retains the unaffected interior. The regression fails
before the fix and passes through movement, reversals, wraparound, parent-only
movement, and rebasing. Release build and live movement smoke test passed;
the user's visual confirmation closes this issue. The earlier streaming-latency
diagnosis was superseded; the committed terrain frame is already transformed
into the current observer frame correctly.

### ✅ River/terrain resolution mismatch ("terrain filling the river") — one of two causes
River and lake water surfaces are generated from a hydrology grid that ran at
one fixed resolution across an entire ~295 km region tile (~3 km between grid
cells). Between two hydrology nodes that far apart, the river's straight-line
interpolated surface couldn't track the actual terrain undulation, so the
ground would rise back up through the water mid-river — visible as a river
that fades into disconnected floating fragments.
**Fix**: added a second, finer-tiled region cache (level 9, ~18 km tiles,
same fixed grid resolution → ~16x finer real-world spacing) that both
`DerivedRegionTerrainSource` (ground elevation) and `RiverWaterRenderer`
(water surfaces) now prefer over the coarse cache wherever it has ready
coverage near the observer, falling back to the coarse cache beyond that
neighborhood. This improved river resolution, but the earlier claim that it
also resolved lake/coastline continuity was contradicted by subsequent user
testing. See the standing-water issue above; lakes now use a different path.
**Related rendering issue:** stale morph data also distorted terrain heights
while moving. That bug is now fixed and user-confirmed above. Its specific
contribution to river occlusion was not separately established.

### ✅ Terrain page cache thrashing near cube-face boundaries
`TerrainPageCache::PruneFarPages` (proactive distance-based eviction, added
this session) sized its keep-radius from each *candidate* tile's own
`ApproximateTileWidthMeters` — but the cube-sphere projection is non-uniform,
so a tile near a cube face's edge/corner can report a width many times
smaller than a same-level tile near a face center, pathologically shrinking
the keep radius. Measured: ~65,000 evictions in 8 seconds against a ~7,500
entry cache with the observer barely moving.
**Fix**: size the keep-radius from the *observer's own* same-level tile width
instead (stable, always centered on real camera position, memoized per
level). Verified: `evict=0` in the same scenario after the fix.

### ✅ Terrain page cache saturation during sustained movement
A 256 MB page cache filled completely (100% budget, 800+ evictions) after a
~10-second, 100 km climb, because pages were only ever evicted once the
budget was already exhausted — a one-directional flight just accumulates
every tile it passes near.
**Fix**: (1) raised the budget 256 MB → 512 MB; (2) halved per-sample storage
(new `CachedTerrainSample`: `f32` elevation + 8-bit quantized biome weights,
32 B vs 64 B) with no change to any consumer outside `terrain_cache`; (3)
added `PruneFarPages` for proactive distance-based eviction instead of
waiting for the budget ceiling. Verified: the same climb now holds flat at
~20 MB/512 MB.

### ✅ Camera could fly through terrain; speed didn't scale with altitude
The free camera used a flat altitude floor (250 m above the base sphere, not
actual sampled terrain) and a fixed move speed regardless of height.
**Fix**: move speed now scales log-log with altitude above the *actual*
ground (~7 km/h near the surface up to a fast orbital cruise near the 2,000 km
ceiling), and the observer is clamped against sampled terrain elevation
(`AnalyticTerrainSource::Sample`) instead of a fixed floor.

### ✅ Orbit crashing at launch
An early `return` inside an `if` block in the `SurfaceDirectionForOffset`
HLSL vertex shader tripped DXC's uninitialized-variable check (X4000) under
warnings-as-errors, failing shader compilation before the window ever drew a
frame. Same bug class as an earlier fix in `VersionOverlayRenderer.cpp`'s
glyph shader.
**Fix**: rewritten to single-exit style (one declared variable, conditionally
reassigned instead of an early return). Commit `750e6049`.

### ✅ Dev-server screenshots captured stale/wrong content
`SCREENSHOT` used `GetDC(hwnd)` + `BitBlt`, which doesn't see anything drawn
through a D3D12 flip-model swapchain — it read whatever was left in the
window's unused GDI surface, which could be entirely unrelated content
(observed: a YouTube video, another game). A follow-up fix (raising the
window to the foreground) also failed silently because blocking the render
thread with `Sleep()` prevented the very frame it needed from ever
presenting.
**Fix**: capture from the desktop DC at the window's screen position
(reads real composited pixels), raise the window's Z-order (not focus, which
Windows blocks for background processes) without blocking, and defer the
actual pixel read to a later frame so the render loop gets to present first.
Commit `750e6049`.
