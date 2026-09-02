# Water Phase 3 — terrain-aware coarse -> fine prolongation

Branch: `water/0.1.0`

## Motivation

The first transactional promotion path seeded a coarse surface-water parcel as one
uniform depth over the whole fine tile. That is conservative, but on uneven terrain
it creates an artificial initial free-surface slope and can wet high cells that
should initially remain dry.

Promotion now preserves the same exact-volume ownership transaction while changing
only the spatial reconstruction inside the unpublished fine tile.

## New GPU prolongation

Added:

```text
shaders/water/hydro_coarse_prolongation.glsl
scripts/water/hydro_coarse_prolongation_gpu.gd
```

After `HydroTerrainBedGPU` stages the real dry destination bed into atlas A+B, the
prolongation kernel solves a level water surface satisfying:

```text
sum(max(eta - bed_i, 0)) * dx^2 = represented_volume_m3
```

The solve is deliberately one GPU invocation for one promotion. Promotion is a
low-cadence ownership event, so a serial tile solve is preferable to atomics,
additional readbacks or a more complex multi-pass scheduler.

### Numerical frame

The kernel solves elevation relative to the minimum tile bed:

```text
rel_bed_i = bed_i - min_bed
eta_rel   = eta - min_bed
h_i       = max(eta_rel - rel_bed_i, 0)
```

This avoids subtracting two large absolute planet elevations repeatedly when the
water depth may only be millimetres or centimetres.

A fixed 32-iteration bisection brackets the free surface. The residual FP32 depth
sum is then applied to the deepest cell so the stored parcel is as close as possible
to the exact target while preserving the wet/dry classification of every other
cell.

A and B are written identically:

```text
h  = terrain-aware reconstructed depth
hu = h * local_velocity.x
hv = h * local_velocity.y
bed unchanged
```

## Ownership quantization

`HydroCoarseProlongationGPU.plan_volume()` quantizes the **physical volume itself**
to FP32 rather than quantizing a uniform depth. If nearest-FP32 rounding would land
above the caller's requested amount, the positive float bit pattern is stepped down
by one ULP.

Therefore:

```text
represented_volume_m3 <= requested_volume_m3
```

and the coarse ownership store can safely reserve/debit exactly the parcel that the
GPU reconstruction targets.

`PlanetHydroPromotionBridge` now uses this prolongation component. Its public
transaction API is unchanged. The completion report keeps `seed_depth_m` as an
**equivalent uniform depth** for backwards/debug comparison and adds:

```text
seed_strategy = "level_free_surface"
```

The authoritative ownership field remains:

```text
represented_volume_m3
```

Frontier fine->fine pre-seeding is intentionally unchanged; that path represents
interface wetting and has different physical semantics.

## GPU gate

Added:

```text
tests/water/HydroCoarseProlongationGPU.tscn
tests/water/test_hydro_coarse_prolongation_gpu.gd
```

The fixture uses a stepped/sloped bed around 1200 m absolute elevation and a parcel
that cannot wet the full tile. It verifies:

- represented volume matches the planned physical parcel;
- at least one high terrain cell remains dry;
- multiple lower cells become wet;
- all meaningfully wet cells share one free-surface elevation within FP32 tolerance;
- requested local velocity is represented by `hu/h` and `hv/h`;
- dry cells carry no momentum;
- staged bed elevations are unchanged;
- atlas A and B are identical.

Suggested local run:

```text
godot --path . tests/water/HydroCoarseProlongationGPU.tscn
```

The current ChatGPT environment does not contain the project Godot 4.7 executable,
so this GPU gate is implemented but not runtime-passed here.
