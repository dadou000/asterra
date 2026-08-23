# Spherical Geometry Clipmap

This document describes Asterra's active visual terrain architecture.

## Goal

Use one camera-centred spherical geometry clipmap from ground level to orbit. Runtime visual terrain does not use the old CPU quadtree/ChunkBuilder path and no longer depends on streamed visual height pages.

The visual height function is now:

```text
H(position, LOD)
=
2x-upsampled smoothed macro elevation(position)
+
band-limited deterministic GPU procedural detail(position, LOD spacing)
```

Every L0-L14 vertex evaluates this same continuous world-space function. Finer levels simply retain more short-wavelength bands.

## Smooth macro source

The authoritative macro grid remains unchanged. Its runtime-smoothed elevation is resampled to **2x face resolution** for graphics and stored in the existing six-face RF texture array with a cross-face gutter.

The GPU samples that texture with hardware linear filtering. The 2x resample plus linear filtering removes visible macro-cell stepping without altering the underlying generated geography.

Cube faces are only a storage projection. The geometry itself remains a camera-centred spherical clipmap and never has cube-face topology boundaries.

## 4K screen-space target

Reference camera: 3840x2160, vertical FOV 68 degrees.

```text
f_px = 2160 / (2 * tan(68deg / 2)) = 1601.17 px
```

A terrain edge of world length `s` reaches 16 pixels at approximately:

```text
d_16 = s * 1601.17 / 16 = 100.073 * s
```

Because adjacent clipmap levels double vertex spacing, the outgoing fine level should be about 8 px/vertex at handoff so the incoming coarse level is about 16 px/vertex.

The reusable topology is **400 cells across** (401 x 401 logical grid vertices), with a 200-cell radius.

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

Asterra's radius is about 1,000 km, so L14 is enough to cover the maximum visible hemisphere cap.

## Concentric geometry

The logical coordinates use a regular 401 x 401 lattice for cheap `VERTEX_ID` reconstruction, but only circular cells are indexed:

- L0 is a circular disc.
- L1-L14 are circular annuli.
- Morph and sink use radial `length(cell)` distance.
- Outer annuli are split into twelve 30-degree sectors for view culling.

Near/regional levels use the cheaper normalized tangent projection. Planet-scale levels use the spherical exponential map.

## Procedural detail at every LOD

The GPU detail generator uses deterministic 3D world-space value noise. It has fixed absolute wavelength bands spanning the interval below the macro grid.

Each band is weighted by the current clipmap spacing:

```text
weight = smoothstep(4 * spacing, 8 * spacing, wavelength)
```

Therefore:

- L0 retains all representable bands.
- progressively coarser levels smoothly lose short wavelengths;
- far levels converge naturally toward the smoothed macro field;
- no LOD independently invents a different terrain surface;
- no runtime height page has to arrive before detail appears.

The current geometry bands are centred around approximately:

```text
12 km
2.4 km
480 m
96 m
24 m
```

Broad mountainous relief increases the amplitudes of the long bands; detail is reduced close to the sea-level crossing so coastlines remain stable.

## Seam-free handoff

Adjacent levels overlap:

- fine outer radius: 200 cells
- coarse shared boundary: 100 coarse cells
- coarse geometry starts near 88 coarse cells
- fine terrain morphs to the parent over the outer ~12.5%
- coarse geometry is radially sunk inside the overlap

Because parent and child evaluate the same macro + procedural function with different band limits, the morph is between two nearby versions of one surface rather than two separately generated terrain pages.

## Visual streaming

There is no visual height-page streaming anymore.

```text
camera
  -> spherical clipmap L0-L14
  -> interpolated smoothed macro texture
  -> deterministic GPU detail at each vertex
  -> parent morph + radial sinking
  -> final terrain
```

The old `GroundHeightPageAtlas` autoload is replaced by a zero-allocation compatibility shim. This removes the 4096-page atlas, page table, page uploads, refinement blocks, and visual terrain disk requests.

The old velocity visual prefetcher is also disabled. Terrain collision already maintains its own motion-biased local source set.

## Physics

Physics remains CPU-side and does not read back GPU terrain.

The local collision height store now evaluates the same inexpensive deterministic value-noise spectrum on CPU, at the collision page's sample spacing, on top of the authoritative CPU bilinear macro elevation. It uses a separate cache namespace so old `TerrainDetail` pages cannot mix with the new surface.

This keeps runtime CPU synthesis bounded to the small physics bubble while the complete visible planet remains GPU-generated.

## Runtime architecture

```text
                 smoothed macro world
                        |
                2x graphics resample
                        |
                        v
                 GPU macro texture
                        |
           +------------+-------------+
           |                          |
           v                          v
 spherical visual clipmap       CPU collision bubble
 L0-L14 procedural GPU          same cheap spectrum
           |                          |
           +------------+-------------+
                        |
                deterministic terrain
```

`FastPlanetTerrain` and `PlanetTerrain` remain compatibility shells only; they do not build visual terrain geometry.
