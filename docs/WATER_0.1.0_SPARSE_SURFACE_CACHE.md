# Water 0.1.0 — Sparse hydrology surface cache

Branch: `water/0.1.0`

## Purpose

The physical sparse atlas is planet-persistent and camera-independent. Rendering
needs a small, dense local field that shaders can sample cheaply. These are kept as
separate representations:

```text
SparseHydroAtlasGPU                  authoritative physics
  h / hu / hv / bed
  stable tile metadata
  occupancy
        |
        | GPU-only reconstruction
        v
WaterSurfaceResources               disposable local render cache
  256 x 256 RGBA32F
  +/- 2048 m around a planet-space anchor
```

Camera/ocean movement may rebuild or recenter the lower representation. It never
allocates, releases, wakes, sleeps or advances a physical hydrology tile.

## GPU lookup

A dense output texture must not scan every resident sparse tile for every texel.
The reconstruction therefore builds an ephemeral GPU hash table each update:

```text
occupied slot metadata
(face, level, x, y)
        |
        v
sparse_hydro_surface_hash.glsl
        |
        v
key -> transient slot hash
        |
        v
256 x 256 output texels
        |
        +--> tangent-plane point -> planet direction
        +--> direction -> equi-angular cube face/UV
        +--> UV -> HydroTileKey at production metric level
        +--> hash lookup -> resident slot
        +--> sample canonical atlas A
```

At the default 1024-slot capacity the hash has 8192 uint entries, approximately
32 KiB. It is rebuilt from occupancy/metadata immediately before reconstruction,
so recycled-slot identity cannot persist in the render cache.

Files:

```text
shaders/water/sparse_hydro_surface_hash.glsl
shaders/water/sparse_hydro_surface_reconstruct.glsl
scripts/water/sparse_hydro_surface_reconstruction_gpu.gd
```

## Output contract

For a resident wet cell:

```text
R = eta = bed + h [m above planet mean surface]
G = velocity along dynamic-cache tangent X [m/s]
B = velocity along dynamic-cache tangent Y [m/s]
A = hydraulic activity hint [0..1]
```

Missing, unoccupied, stale or dry cells write zero.

Velocity is transformed from the hydrology tile's local cube-face axes into the
persistent render-cache tangent frame. This matters across cube faces: a physically
continuous current cannot be interpreted as the same raw `(hu,hv)` pair on every
face.

## Production cache bridge

Added autoload:

```text
SparseHydroSurfaceCache="*res://scripts/water/sparse_hydro_surface_cache_bridge.gd"
```

It is loaded immediately after `WaterSystem` and has process priority 13, after:

```text
OceanSystem                  priority 10
WaterSystem                  priority 11
SparseHydrologyRuntime       priority 12
SparseHydroSurfaceCache      priority 13
```

The bridge observes `SparseHydrologyRuntime.cycle_completed` and refreshes the
local render cache. It may reanchor the **cache only** toward OceanSystem's current
surface center after the observer moves by a configurable fraction of the cache
half-extent.

No physical API is called during cache reanchoring.

## Ocean rendering boundary

The existing ocean coat samples the cache and interprets R as absolute radial water
surface elevation relative to mean sea level. This works for coastal surge, wakes,
flood fronts reaching the coast and other ocean-connected hydrology.

The ocean shader intentionally discards land. Therefore an inland lake/flood with
`eta=120 m` may exist correctly in the cache but is not drawn by the ocean coat.
That is desirable: inland/flood rendering needs a separate local-water surface coat
rather than teaching the ocean clipmap to render through terrain.

## Validation gate

Added:

```text
tests/water/SparseHydroSurfaceReconstructionSmoke.tscn
tests/water/test_sparse_hydro_surface_reconstruction.gd
```

The renderer-mode test creates two adjacent resident sparse tiles plus one stale,
unoccupied high-value slot and reconstructs the shared cache. Test-only texture
readback checks:

- both resident tile free-surface elevations appear;
- velocity remains finite and transformed into the cache frame;
- activity is nonzero for moving water;
- the stale unoccupied slot cannot leak;
- texels outside resident sparse coverage remain zero.

The current ChatGPT environment does not contain the project's Godot 4.7 renderer
executable, so this test is implemented/static-reviewed but not runtime-passed here.

Suggested local run:

```text
godot --path . tests/water/SparseHydroSurfaceReconstructionSmoke.tscn
```

## Next renderer integration

Add a separate inland/flood surface coat over terrain:

```text
same WaterSurfaceResources cache
        |
        v
local terrain-following water mesh
        |
        +--> eta > terrain + wet threshold => visible
        +--> eta <= terrain               => discard
        +--> ocean/land handoff avoids double rendering
```

This surface is still only a rendering representation. Sparse hydrology remains the
physical authority.
