# Water 0.1.0 — Persistent 1D river / reach hydrology

Branch: `water/0.1.0`

## Why this layer exists

The planet-wide coarse hydrology originally routed every channel cell as a lumped
0D reservoir. That is appropriate for tiny tributaries and background drainage,
but it loses the physical distinction between a river contained inside its banks
and water that has spilled onto a floodplain.

Generated macro rivers already have stable world data:

```text
flow_dir / PlanetGrid neighbour graph
mean discharge Q
river_width
Strahler stream_order
floodplain
terrain elevation
```

The runtime now promotes those generated macro rivers to a persistent 1D channel
representation while keeping smaller drainage as cheap lumped storage.

## Representation ladder

```text
small tributary / drainage cell
        |
        v
0D channel reservoir
        |
        | enters generated macro river
        v
1D Manning reach
        |
        | exceeds bankfull
        v
coarse surface / floodplain storage
        |
        | surface flood promotion policy
        v
sparse 2D SWE
```

Channel-only anomalies therefore no longer need to be treated as flat flood
parcels merely because discharge is high.

## Runtime classes

Added:

```text
scripts/water/planet_river_reach_store.gd
scripts/water/planet_hydrology_reach_ownership_store.gd
scripts/water/persistent_hydrology_reach_system.gd
```

`PersistentHydrologySystem` keeps the same autoload name and public API. The project
autoload now points to the thin reach-aware facade, which constructs
`PlanetHydrologyReachOwnershipStore` instead of changing existing users.

The original `PlanetHydrologyStore` and `PlanetHydrologyOwnershipStore` remain
available for focused numerical ownership tests.

## Reach definition

The first implementation uses one generated macro river cell as one 1D reach
segment.

A cell becomes a reach when:

```text
land elevation > sea level
river_width > 0
not a lake cell
has a valid downstream receiver
```

`PassHydrology` currently sets `river_width > 0` at approximately `Q >= 1.5 m3/s`,
so sub-grid streams remain in the cheaper lumped representation.

The reach length is the spherical centre-to-receiver distance. The generated
`river_width` becomes the trapezoid bottom width.

## Cross-section calibration

Each reach uses a trapezoidal cross-section:

```text
             water
      -----------------
        /           \
       /             \
      -----------------
          bottom b
```

with area:

```text
A(h) = h * (b + z*h)
```

and wetted perimeter:

```text
P(h) = b + 2*h*sqrt(1 + z^2)
```

Manning discharge is:

```text
Q(h) = (1/n) * A * R^(2/3) * sqrt(S)
R = A/P
```

The runtime solves two static depths during initialization:

1. **normal depth** — reproduces the generated mean annual discharge;
2. **bankfull depth** — reproduces a larger configurable discharge, currently
   `2.5 * baseline Q`.

The channel bed is placed so:

```text
channel bed + bankfull depth = coarse terrain elevation
```

Therefore bankfull stage corresponds to the local floodplain/ground elevation in
the macro model.

## Dynamic ownership

The reach layer does **not** introduce another dynamic water reservoir.

Authoritative river volume remains:

```text
PlanetHydrologyStore.channel_storage_m3[cell]
```

`PlanetRiverReachStore` owns only static geometry plus deterministic conversions:

```text
storage -> depth
storage -> stage
storage -> Manning capacity
storage + dt -> one-hop outflow
```

This is important because the existing transactional coarse/fine ownership ledger
continues to operate on the same physical water array.

## Routing

At every coarse hydrology step:

```text
rain / infiltration / baseflow
        |
surface runoff -> channel inflow
        |
        v
reach storage
        |
        +-- if storage > bankfull --> coarse surface flood storage
        |
        v
Manning capacity
        + finite reach travel time
        |
        v
one-hop downstream outflow
```

Outflow is bounded by:

```text
available reach volume
Manning conveyance over dt
finite kinematic travel fraction over reach length
```

Downstream inflow is still buffered until all source cells have advanced. Water
therefore cannot cross an arbitrary number of macro reaches in one large time-warp
step.

## Bank overflow

Storage above the calibrated bankfull volume is not hidden inside the 1D channel.
It is transferred conservatively to:

```text
surface_storage_m3[cell]
```

This is an internal ownership movement, not an external source/sink, so total water
and the coarse mass ledger are unchanged.

That surface parcel can then trigger the existing surface/flood sparse-SWE
promotion path.

## Channel anomaly query

The reach-aware production store exposes:

```gdscript
channel_reach_candidates(
    max_count,
    discharge_ratio_threshold,
    bankfull_ratio_threshold
)
```

Candidates contain:

```text
cell / receiver
discharge and baseline discharge
discharge ratio
river width
river depth / stage
bankfull depth / ratio
stream order
channel storage
```

This query is deliberately separate from automatic surface promotion.

**No automatic channel-only 1D -> 2D promotion is enabled yet.**

The next bridge must reconstruct a narrow river corridor aligned with the reach and
must reserve/debit channel storage specifically; a high-Q river must not be seeded
as a level flood sheet over an entire fine tile.

## Production integration

`project.godot` keeps the global singleton name:

```text
PersistentHydrologySystem
```

but points it to:

```text
scripts/water/persistent_hydrology_reach_system.gd
```

Existing weather coupling, rainfall authority, ownership transactions, automatic
surface promotion/demotion and representation audits therefore keep the same API.

## CPU gate

Added:

```text
tests/water/PlanetRiverReachStoreTests.tscn
tests/water/test_planet_river_reach_store.gd
```

The gate verifies:

1. generated-width river cells become 1D reaches;
2. solved normal depth reproduces generated baseline discharge;
3. bankfull depth is above normal depth;
4. production initialization uses the calibrated normal-flow storage;
5. downstream routing debits and credits exactly the same parcel;
6. water moves at most one macro receiver per coarse step;
7. above-bankfull volume is transferred exactly to surface storage;
8. total coarse water and mass ledger remain closed;
9. the channel-only anomaly query identifies the stressed reach.

Suggested run:

```bash
godot --headless --path . tests/water/PlanetRiverReachStoreTests.tscn
```

The current ChatGPT execution environment does not contain the project's Godot 4.7
executable, so this gate is implemented but not runtime-passed here.

## Next channel-specific work

The next Phase 3 river step should be a **reach-aware 1D -> sparse 2D bridge**:

```text
abnormal 1D reach
        |
reserve channel parcel
        |
construct reach centreline through fine tile(s)
        |
terrain-aware narrow-channel seed
        |
publish connected SWE corridor
        |
1D reach keeps only the unpromoted channel parcel
```

Required before enabling it automatically:

- channel-specific ownership reservation rather than surface-first reservation;
- fine-cell corridor/ribbon reconstruction from the reach centreline;
- velocity aligned with downstream reach tangent;
- exact GPU volume acknowledgement;
- reverse 2D -> 1D restriction for quiet river corridors;
- junction handling so tributary/confluence water is not duplicated.
