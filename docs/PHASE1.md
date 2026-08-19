# Phase 1 design notes

Why each system works the way it does, and what it costs.

---

## Coordinates: hierarchical frames and a floating origin

`Helion frame → Asterra frame → regional frame → local physics frame → assembly frame`

Godot's `Vector3` is 32-bit in standard builds. At a 1000 km radius that is about
0.1 m of positional resolution at the surface — fatal for a 0.75 m edit lattice
and visible as vertex swim long before that. So:

* canonical positions are `Vec3D` (GDScript `float` is a 64-bit double);
* `Frames.origin` is re-based onto the observer whenever it drifts past 4 km;
* every chunk stores its double-precision pivot and is re-projected on rebase;
* chunk vertices are generated *relative to that pivot*, so the float32 numbers
  the GPU sees are never larger than a chunk.

The test suite checks that a 1 cm offset survives at 1.6 × 10⁶ m from the origin.

## The cube-sphere

Equi-angular (`tan`/`atan`) rather than gnomonic or the "adjusted cube" mapping.
Equi-angular keeps cell areas within ~1.3× across a face *and* inverts in closed
form, which the streaming quadtree, the terrain-delta lattice and the excavator
all need — they must be able to go from a world direction back to a face and a
cell, not just forwards.

Cross-face adjacency is resolved geometrically: a neighbour that falls off the
edge of a face is re-projected through the inverse mapping. There is no seam
special-casing anywhere in the codebase, and the 8 cube corners (24 cells) are the
only places where a cell has 7 neighbours instead of 8.

## Macro geography, and why hypsometry needed fixing

Plates are a warped nearest-seed partition; margins carry a convergence sign from
the relative drift of the two plates, so orogens, rifts, ridges and trenches all
follow from one number. Continental crust is buoyant, oceanic crust is not, and a
fractal coastal noise perturbs the land/sea threshold so coastlines are irregular.

The first version of this produced a **bimodal elevation histogram with a gap in
it**: continents sat 3 km above sea level on average and oceans were 500 m deep.
A plate model is good at deciding *where* crust is high; it is bad at deciding
*how* high. The fix is a **hypsometric remap** — every cell's rank in the global
elevation distribution is looked up in a target curve (abyssal plains, continental
rise, shelf, coastal plain, uplands, orogen). Because it is a monotone transform,
every divide, slope and drainage direction the tectonics produced is preserved
exactly; only the vertical scale changes. It also makes `ocean_fraction` exact
instead of approximate.

Result: mean land elevation 884 m (Earth ≈ 840 m), mean ocean depth −3278 m.

## Geology before soil

Rock family is chosen from tectonic setting, not painted: mafic crust under the
deep ocean, metamorphic cores and granite plutons in orogens, carbonates in warm
shallow basins, clastics elsewhere. Resources then need a *reason*:

* banded iron in old cratonic and metamorphic terrain;
* porphyry copper in volcanic arcs above subduction zones, enriched along faults;
* coal where a basin was swampy; petroleum where a basin had organic source rock,
  burial and a seal, with the gas fraction rising with burial depth;
* high-purity quartz in quartzite and pegmatitic granite margins — Axiom's
  semiconductor industry has to come from somewhere;
* aquifers in porous and karstic rock, damped by relief.

Each family also carries an erodibility and a clay yield, which the erosion and
soil passes consume. Erodibility is additionally stored as a *continuous* field:
sampling it per-cell with nearest-neighbour lookup put a visible 8 km terrace
grid into the runtime terrain, because it drives detail amplitude.

## Erosion: implicit stream power

An explicit stream-power solver needs thousands of small timesteps. The implicit
form (Braun & Willett 2013) is unconditionally stable, so ~20 large steps produce
a drainage-carved landscape — which is what makes an offline pass affordable
inside a game bake.

Each iteration: apply tectonic uplift → priority-flood the depressions → compute
D8 receivers → build an O(N) traversal order from the receiver tree (no sorting)
→ implicit incision from outlets upstream → route the sediment flux → hillslope
relaxation.

**Closed basins were the subtle failure.** The solver cannot incise a cell with no
real downhill receiver, so every local minimum in the initial noise survived to
the end and became a lake: 18 % of the land was underwater. The fix is to silt up
depressions *inside* the loop — pits shallower than ~14 m fill completely each
iteration, deeper ones lose 30 % of the remainder — so only basins that tectonics
is actively rimming survive. Lakes now cover 4.9 % of land with a median depth of
7 m.

## Hydrology

Discharge is rainfall-weighted accumulation over the same flow tree, converted to
m³/s. Watersheds are outlet ids propagated upstream; Strahler order is computed in
one reverse pass. River width follows the regime relation `w ≈ 7.2·√Q`; anything
below a few m³/s is a stream that lives below the macro grid and is synthesised
locally at runtime.

A lake additionally requires a **positive water balance** — closed basins in dry
climates stay dry ground rather than becoming implausible desert lakes.

## Climate runs twice

Erosion needs rainfall (wet windward slopes really do incise faster), and the
final rainfall field needs the eroded mountains to cast their rain shadows. So
`PassClimate` runs once on the raw crust and again on the finished surface.

Temperature falls as sin²(latitude) — the insolation profile — with `polar_bias`
steepening it, which is what makes Asterra's ice caps larger than Earth's.
Rainfall is advected moisture: a Dijkstra distance-to-ocean gives the moisture
supply, an upwind march of 12 cells gives the orographic barrier, and the
three-cell circulation supplies the wind directions that both depend on. Severe
weather comes out of the same state: tropical cyclogenesis over warm water away
from the equator, and continental convective outbreaks downwind of a mountain
barrier where warm moist inflow meets a dry elevated layer.

## Soil and biomes are derived, not painted

Soil depth is weathering intensity (warm × wet) modulated by slope retention,
plus floodplain and sediment deposition. Texture comes from the parent rock's clay
yield, its quartz content, and transported silt. Organic matter is production over
decomposition, so peat forms in cold wet ground. Horizons (O/A/B/C/bedrock) are
derived from those numbers and are exactly what the excavator reads.

Biomes are a Whittaker temperature/precipitation classification overridden by
whatever physically dominates: standing water, permanent ice, wetland, treeline,
bare rock.

## Infrastructure suitability

Computed *before* any city exists, so settlement can later be explained by
geography rather than the other way round. Broad valleys, river corridors with a
gentle downstream gradient, mountain passes (saddles that sit low inside high
country), navigable coast, and flat plains — smoothed lightly, because a corridor
that does not connect is not a corridor. This is the field that will locate
Sterling's rail valley: it is not placed, it is found.

## Streaming

The quadtree splits when a node is within `lod_split_factor` node-widths of the
observer, and rejects nodes beyond the horizon — computed from the observer's
altitude plus the tallest terrain that could be poking over the curve. On a planet
that removes most of the surface from consideration and cut the ground-level chunk
count by roughly a third.

Chunk meshing runs on `WorkerThreadPool`, and a parent's mesh is held until all
four children are ready, so the surface never opens a hole while streaming.
Skirts hide the remaining LOD cracks.

The macro fields vary on an 8 km grid, so they are sampled on a coarse per-chunk
lattice and interpolated; only the detail noise and the player's deltas are
evaluated per vertex. On a 24 m chunk that is the difference between seven
cube-sphere inversions per vertex and none — about a 2× reduction in fill time.

**Detail amplitude has to be capped against wavelength, not just relief.** The
first version scaled amplitude to macro relief alone, which put 620 m of fBm on a
2.8 km feature — near-vertical ground everywhere in the mountains, and a seam test
that failed for the right reason.

## Editing and persistence

Edits are height offsets on a fixed cube-sphere lattice (2²¹ samples per face
edge, ≈ 0.75 m on a 1000 km planet), grouped into 64×64 tiles. Untouched ground
costs zero bytes; a save file scales with how much the player actually dug.

Excavation charges each slice of the brush to the material it came from, and
produces *loose* (post-bulking) cubic metres — because that is what a bucket
holds. Filling consumes real stock and converts back through the bulking factor.
The editor cannot invent matter, and the test suite asserts it.

Loose heaps carry their material's density, bulking factor and angle of repose,
and their cone geometry follows the repose angle — clay stands steeper than sand,
so the same volume makes a different heap.

The save holds the seed, the player, the deltas and the heaps. The planet is never
serialised; it comes back from the seed. That is the property the Phase 1
milestone actually tests.
