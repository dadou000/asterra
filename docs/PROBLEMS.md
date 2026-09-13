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

### ⚠️ Terrain clipmap ring overlap / gaps
Adjacent clipmap rings (or the coarse/fine tile boundaries within the terrain
streaming system) may not hand off cleanly — visible as either a seam where
neither ring draws (a gap) or two rings drawing the same area (overlap /
z-fighting). Related recent work: `66d0d21f Test clipmap ring morph-band
overlap`, `2a048d65 Align coarse ring overlap to finer morph band`. Not yet
isolated to a specific ring pair or altitude band — needs a targeted repro
(fly through a ring boundary at a fixed altitude, screenshot before/after the
transition, check for a hard edge).

**Also causes terrain to fill in over rivers**: where a ring overlaps, the
terrain drawn from the wrong (or a conflicting) ring can sit above the actual
river/lake water surface, swallowing it the same way the hydrology
resolution mismatch below did. This is a second, separate cause of that same
"river disappears into the ground" symptom — fixing the hydrology resolution
mismatch did not fix this one, since it's a rendering-time ring conflict, not
a simulation-resolution gap. Not yet fixed.

### 🔍 Soft brightness band on terrain, ~19–56 km altitude
A faint, non-hard-edged brightness/color discontinuity visible on the terrain
surface specifically in the 19–56 km altitude range (seen in two separate
`SLEW` captures at the same spot, both altitudes inside that band, not seen
below or above it). Could be the clipmap morph band overlap above, or an
unrelated lighting artifact — not isolated. Not seen during the ground-level
(300 m) or mildly-high (4 km) clipmap movement test, which stayed clean.

---

## Fixed

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
neighborhood. Verified: lake/coastline shorelines render continuous and
detailed with no gaps in multiple test locations.
**Not the whole story**: the clipmap ring overlap/gap problem above produces
the same "terrain fills the river" symptom through a different mechanism
(a rendering-time ring conflict, not a simulation-resolution gap), and is
still open — so this symptom can still occur.

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
