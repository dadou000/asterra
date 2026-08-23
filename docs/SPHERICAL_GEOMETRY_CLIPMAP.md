# Spherical Geometry Clipmap

This document is the implementation target for Asterra's visual terrain renderer.

## Goal

Use one camera-centred spherical geometry clipmap from ground level to orbit. Runtime visual terrain must not use the old CPU quadtree/ChunkBuilder path.

The clipmap is displaced entirely on the GPU from the persistent sparse height-page pyramid. Cube faces are storage only; geometry is not split at cube-face boundaries.

## 4K screen-space target

Reference camera: 3840x2160, vertical FOV 68 degrees.

Pixel focal length:

```text
f_px = 2160 / (2 * tan(68deg / 2)) = 1601.17 px
```

A terrain edge of world length `s` reaches 16 pixels at approximately:

```text
d_16 = s * 1601.17 / 16 = 100.073 * s
```

Because adjacent clipmap levels double vertex spacing, the outgoing fine level should be about 8 px/vertex at the handoff so the incoming coarse level is about 16 px/vertex.

The exact reusable topology is therefore **400 x 400 cells** (401 x 401 logical vertices), not 192 x 192. 192 cells would put the fine level near 16.7 px/vertex at its edge but the incoming 2x-coarser level near 33.4 px/vertex. A 400-cell grid gives a 200-cell half-width, which puts the 2x-coarser level at approximately 16 px/vertex at the shared handoff.

With the current ~0.75 m finest spacing, nominal handoff radii are:

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

Asterra's radius is about 1,000 km, so the maximum visible surface cap approaches `pi*R/2 ~= 1,571 km`. L14 is therefore sufficient to cover the visible hemisphere.

## Geometry

- L0 is one full square grid.
- L1-L14 share one annular strip topology and are drawn as instances.
- Geometry is projected with the spherical exponential map, not a large tangent-plane approximation:

```text
arc = length(offset)
theta = arc / planet_radius
tangent = normalize(right * offset.x + up * offset.y)
dir = center * cos(theta) + tangent * sin(theta)
position = dir * (planet_radius + height)
```

Only the visible spherical cap is required. The active outer level follows camera altitude/horizon distance.

## Seam-free handoff

Adjacent levels overlap. The inner edge of the coarse ring begins inside the outer edge of the fine ring.

For the 400-cell topology:

- nominal fine outer radius: 200 cells
- coarse nominal shared boundary: 100 coarse cells
- coarse geometry begins at about 88 coarse cells (~12% overlap)
- fine height morphs toward the parent over approximately the last 12.5% of its radius
- coarse geometry is radially sunk at its inner edge and rises to the true parent surface at the shared boundary

At the exact shared boundary the fine level has morphed to the parent height and the coarse level has zero sink, so both surfaces agree. Inside the overlap, the coarse surface is deliberately behind the fine surface.

The sink is radial:

```text
position = dir * (planet_radius + height - sink)
```

No quadtree stitching, skirts, T-junction repair, or cube-face geometry seams are required.

## Height data

The persistent 33x33 sparse page system becomes planet-wide:

```text
L0  ~0.75 m
L1  ~1.5 m
...
L14 ~12.3 km
```

Runtime lookup order is:

```text
requested sparse level -> progressively coarser sparse parent -> orbit macro texture fallback
```

The orbit texture is an immediate safety fallback while new sparse pages are still loading or being compiled. Missing pages therefore reduce detail instead of creating holes.

Target cache sizes for the 15-level renderer:

- GPU atlas: 4096 pages (64 x 64 slots, 33 x 33 texels each), about 18 MiB of RGBAF/RF payload scale depending on backend storage
- GPU page table: 16384 entries
- RAM page cache: 8192 pages

Visible pages are demand touched and protected. The page-table metadata must always be copied with blending disabled.

## Shading

The vertex shader performs one centre height lookup per vertex. Terrain normals are reconstructed in the fragment shader with screen-space derivatives. The previous five `planet_position()` evaluations per vertex are not used.

This is required because a 400-cell, 15-level clipmap is intentionally vertex-rich but still cheap enough when vertex work is one sparse height path rather than five.

## Runtime visual architecture

```text
camera
  -> snapped spherical clipmap centre
  -> L0 + active L1..Ln static instances
  -> sparse GPU page lookup
  -> parent fallback
  -> orbit macro fallback
  -> radial morph + sink
  -> final planet surface
```

`FastPlanetTerrain`/`PlanetTerrain` remain compatibility shells for harness and collision ownership only. They must not create visual terrain geometry.

## Physics

Physics remains CPU-side and reads the same `GroundHeightStore` pages. Visual GPU terrain and CPU collision share one authoritative height dataset; no GPU readback is required.
