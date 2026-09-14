# Standing water on the terrain surface

Implemented 2026-09-13. Automated validation passed; user visual confirmation is pending.

## Failure mechanism

The screenshot's HUD showed 14,337 ocean vertices and zero lake cells. Ocean
vertices lay on the sea sphere, but rasterization joined them with flat chords.
The terrain used another, independently changing grid. Their chords intersected,
exposing alternating water/terrain bands even where both analytic surfaces were
well behaved. Increasing ocean subdivisions alone would not make the two meshes
agree at every terrain LOD.

Inland lakes had additional problems: independently solved coarse/fine basin
levels, quads ending half a hydrology cell from each wet sample, shallow cells
excluded by the 1 m extraction threshold, and a hard per-frame 8,192-cell cap.
Fine terrain also retained frequencies absent from the bed used to solve a lake,
so it could emerge through that lake's surface.

## Current representation

`TerrainSample.elevationMeters` remains the ground/bed for CPU queries.
`standingWaterDepthMeters` is nonnegative. The renderer draws their sum as one
opaque exterior surface. The same triangles, spherical projection, toroidal
residency, morph targets, clipmap holes and depth write now serve land and water.
There is no detached water sheet to stop before the shore or intersect another
LOD's chord. Shallow/deep coloring and view-angle reflection shading use water
depth and the local sphere normal.

Both bed and depth interpolate linearly through page filtering and morphing.
This detail matters: interpolating a wet bed of -40 m and dry bed of +20 m and
then clamping to sea level gives a different result from interpolating the
parent's already-filled surface. Carrying depth separately preserves the latter.

`LakeWaterField` contains a dense support grid covering the hydrology overlap.
Minimum depth and cell count qualify a basin, but no longer delete its shallow
fringe. Four neighbouring samples give a constant-time bed and water-level
lookup; level interpolation uses only wet corners. Water ends where the
interpolated bed reaches that level. One dry-cell bank around the basin blends
its hydrology bed back into surrounding terrain, suppressing unresolved relief
that would otherwise protrude through water. Ground queries use this same bed.

The coarse region cache owns standing basins. Fine cache arrivals can refine
rivers and surrounding terrain but cannot replace a lake with a separately
solved fine-region level. Existing regional overlap and footprint fades remain;
water and its bed fade together. Generator revisions invalidate the old recipe.

The sandbox no longer constructs/draws `OceanRenderer`. Legacy lake cell drawing
is opt-in (`maximumLakeCells`, default zero); rivers retain their separate pass.
The legacy ocean class remains available for other clients. Do not combine it
with this filled terrain surface: that recreates the independent-mesh problem.

## Cost and validation

- Packed page samples: 32 -> 36 bytes (+12.5%); the page cache budgets actual size.
- GPU samples: 20 -> 24 bytes (+20%), about 198 KiB additional resident data for
  50,700 samples. Heights/normals also read depth; this is not a free GPU change.
- Additional lake support: 9 bytes per hydrology sample (two floats and a byte),
  approximately 146 KiB for a 129-by-129 region, allocated during generation.
- Separate lake uploads at the old 8,192-cell cap: 655,360 bytes/frame removed,
  along with 32,768 per-frame lake-corner conversions. This is a byte/work count,
  not a claimed measured FPS improvement. Extra sampling happens during terrain
  streaming, and standing-water shading adds pixel work.
- The independent ocean draw, index buffer and wave vertex work are absent.
- Release sandbox startup compiled the embedded shaders successfully. Runtime
  checks at 2,000 km, 300 m and a low-altitude slew retained 12 terrain draws,
  settled region queues and no logged errors. These are telemetry checks, not
  screenshots or visual certification.
- All 23 available non-camera tests pass. New coverage checks shallow fringes,
  interpolated shore crossing, overlap, wet/dry cache and morph continuity,
  sea level at four source footprints, fine-region arrival, and the GPU payload.
  The movement regression now checks depth during wraps, parent moves and rebases.
- The existing cache-budget regression now measures `CachedTerrainSample`, which
  is the actual stored representation, instead of the larger public sample.
  The unrelated Camera test still references removed configuration members.

## Remaining limits

This is an opaque heightfield exterior, not a water-volume renderer: underwater
bed rendering, refraction and wave displacement are not provided by this path.
CPU bed/depth remain available for future underwater rendering and simulation.

Near a shore, triangles crossing wet/dry samples approximate the intersection
at the active grid spacing; they remain connected but can form a small sloped
transition. It is not a sub-cell shoreline mesh. Coarse hydrology controls basin
shape and can smooth banks. Independently generated neighbouring regions still
blend in overlaps; globally persistent lake IDs and shared spill levels would be
needed to guarantee a single exact level across arbitrary region boundaries.
Unresolved lakes fade with footprint, so this is not a promise of identical
shoreline detail at every zoom level.
