# Spherical Geometry Clipmap

This document is the implementation target for Asterra's visual terrain renderer.

## Goal

Use one camera-centred spherical geometry clipmap from ground level to orbit. Runtime visual terrain must not use the old CPU quadtree/ChunkBuilder path.

The clipmap is displaced entirely on the GPU from the persistent sparse height-page pyramid. Cube faces are storage only; geometry is not split at cube-face boundaries.

## 4K screen-space target

Reference camera: 3840x2160, vertical FOV 68 degrees.

```text
f_px = 2160 / (2 * tan(68deg / 2)) = 1601.17 px
```

A terrain edge of world length `s` reaches 16 pixels at approximately:

```text
d_16 = s * 1601.17 / 16 = 100.073 * s
```

Because adjacent clipmap levels double vertex spacing, the outgoing fine level should be about 8 px/vertex at the handoff so the incoming coarse level is about 16 px/vertex.

The reusable topology is therefore **400 cells across** (401 x 401 logical grid vertices). A 200-cell radius places the incoming 2x-coarser level at approximately 16 px/vertex at the shared handoff.

| Level | Vertex spacing | Outer/handoff radius |
| ---: | ---: | ---: |
| L0 | ~0.75 m | ~150 m |
| L1 | ~1.5 m | ~300 m |
| L2 | ~3 m | ~600 m |
| L3 | ~6 m | ~1.20 km |
| L4 | ~12 m | ~2.40 km |
| L5 | ~24 m | ~4.80 km |
| L6 | ~48 m | ~9.61 km |
| L7 | ~96 m | ~19.21 km |
| L8 | ~192 m | ~38.43 km |
| L9 | ~384 m | ~76.86 km |
| L10 | ~768 m | ~153.71 km |
| L11 | ~1.536 km | ~307.42 km |
| L12 | ~3.072 km | ~614.85 km |
| L13 | ~6.144 km | ~1,229.7 km |
| L14 | ~12.288 km | ~2,459.4 km |

Asterra's radius is about 1,000 km, so the maximum visible surface cap approaches `pi*R/2 ~= 1,571 km`. L14 is sufficient to cover the visible hemisphere.

## Concentric geometry

The logical vertex coordinates still come from a regular Cartesian 401 x 401 lattice because that is efficient for height sampling and VERTEX_ID reconstruction, but **only circular cells are indexed**:

- L0 is a circular disc with radius 200 cells.
- L1-L14 are circular annuli with outer radius 200 cells and inner radius about 88 cells.
- The morph and sink weights use Euclidean radius `length(cell)`, not `max(abs(x), abs(y))`.

This means the rendered hierarchy is genuinely concentric rather than a stack of square rings.

Geometry is projected onto the sphere. Near-field L0-L6 uses a normalized tangent projection for speed; far levels use the spherical exponential map:

```text
arc = length(offset)
theta = arc / planet_radius
tangent = normalize(right * offset.x + up * offset.y)
dir = center * cos(theta) + tangent * sin(theta)
position = dir * (planet_radius + height)
```

Only the visible spherical cap is required. The active outer level follows camera altitude/horizon distance.

## Seam-free handoff

Adjacent levels overlap:

- fine outer radius: 200 cells
- coarse shared-boundary radius: 100 coarse cells
- coarse geometry begins near 88 coarse cells (~12% overlap)
- fine height morphs toward the parent over the outer 12.5%
- coarse geometry is radially sunk at its inner edge and rises to the true parent surface at the shared boundary

At the shared circular boundary the fine level has morphed to the parent height and coarse sink reaches zero. Inside the overlap the coarse surface remains deliberately behind the fine surface.

```text
position = dir * (planet_radius + height - sink)
```

No quadtree stitching, skirts, T-junction repair, or cube-face geometry seams are required.

## Sector culling

The circular outer annuli are split into **12 static 30-degree sectors**. Each sector contains every currently active L1-L14 instance, so culling one sector removes that angular wedge from all outer LODs in one node.

For ordinary near-horizontal views, only sectors intersecting the camera's horizontal view plus a conservative margin are enabled. Looking steeply toward or away from the planet disables sector culling and renders all sectors to avoid false negatives.

Typical horizontal view:

```text
12 total sectors
~5-7 visible sectors
```

This increases draw-call count slightly but can cut outer-ring vertex submissions close to half. L0 remains a single circular batch.

## Height data and runtime backpressure

The persistent 33 x 33 sparse page hierarchy exists through L14, but real-time visual refinement is intentionally bounded:

```text
L0-L6: sparse GPU pages when already cached/precompiled
L7-L14: orbit elevation texture directly
```

Broad visual requests are RAM/disk-only. They never trigger procedural runtime terrain baking. Missing visual pages therefore reduce detail instead of rebuilding the planet on the CPU.

Collision and small local predictive prefetch may still synthesize missing pages, with strict queue backpressure.

Current GPU cache:

- height atlas: 4096 pages, 64 x 64 slots
- page table: 8192 entries (below Godot DrawableTexture2D's 16383-dimension limit)
- hash probe budget: 12
- page-table changes are batched rather than creating a 1 x 1 ImageTexture per mutation
- atlas eviction scans a bounded rotating window instead of all 4096 slots

## Shading

Near sparse samples use one page-table lookup in the normal same-page bilinear case. The parent height is evaluated only in the outer morph band.

L7+ samples the hardware-linearly-filtered orbit elevation directly. Terrain normals are reconstructed in the fragment shader with screen-space derivatives instead of evaluating neighbouring heights in the vertex shader.

## Runtime visual architecture

```text
camera
  -> snapped spherical clipmap centre
  -> circular L0 disc
  -> view-culled circular L1..Ln sectors
  -> L0-L6 sparse GPU height pages
  -> L7+ orbit elevation
  -> radial parent morph + radial coarse sinking
  -> final spherical surface
```

`FastPlanetTerrain`/`PlanetTerrain` remain compatibility shells for harness and collision ownership only. They do not create visual terrain geometry.

## Physics

Physics remains CPU-side and reads the same `GroundHeightStore` data. Visual GPU terrain and CPU collision share one authoritative height dataset; no GPU readback is required.
