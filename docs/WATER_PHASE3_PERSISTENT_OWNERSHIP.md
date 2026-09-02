# Water Phase 3 — persistent hydrology ownership

Branch: `water/0.1.0`

## Purpose

The planetary coarse hydrology store and sparse fine SWE are different physical
representations of the same water. Promotion is therefore an ownership transfer,
not a source term. A coarse cell must never remain charged with water that has
already been seeded into a fine tile.

## Implemented ownership contract

### Coarse planetary store

`PlanetHydrologyOwnershipStore` extends the existing conservative
`PlanetHydrologyStore` without changing its routing numerics.

Promotion is two-phase:

```text
prepare_promotion(volume)
        |
        | reserve only; coarse physical storage unchanged
        v
fine tile reserve + terrain stage
        |
        v
exact one-shot GPU seed into hidden A+B state
        |
        | seed dispatch recorded
        v
commit_promotion()
        |
        | exact coarse debit
        v
activate_reserved() + publish sparse identity
```

Failure before commit calls `rollback_promotion()`. Because prepare never debits
physical storage, rollback only releases reservation bookkeeping.

While any promotion is unresolved, coarse `step()` returns `ERR_BUSY`. This is a
conservative global barrier preventing reserved water from routing away during an
asynchronous GPU handoff. A future optimization may replace it with per-cell locks.

Pending transactions are not serialized. `snapshot()` returns an empty dictionary
while ownership is unresolved so a save cannot resurrect coarse water after a fine
seed may already have been queued.

Representation transfers have their own ledger:

```text
expected coarse storage =
    initial
  + precipitation
  + climatology input
  + demoted from fine
  - outlet export
  - promoted to fine
```

So promotion/demotion does not masquerade as environmental water creation/loss.

### Exact sparse seed

`HydroCoarseSeedGPU` + `hydro_coarse_seed.glsl` operate only on an unpublished
sparse atlas slot whose terrain has already been staged dry.

The seed is added identically to state A and state B:

```text
h  += dh
hu += dh * u
hv += dh * v
bed unchanged
```

`plan_volume()` quantizes `dh` to the actual FP32 value that will be sent to the
GPU and reports the corresponding represented volume. The ownership transaction
reserves/debits that represented volume, not the original double-precision request.
This prevents a systematic CPU/GPU volume mismatch from FP32 seed quantization.

The initial seed distributes one parcel uniformly over one sparse tile. It is a
conservative reconstruction, not a final lake/river equilibrium reconstruction;
the SWE solver is expected to redistribute it according to the staged terrain.
Future prolongation can replace the spatial distribution while preserving the
same exact-volume acknowledgement contract.

### Transaction bridge

`PlanetHydroPromotionBridge` sequences:

1. resolve or accept a target `HydroTileKey`;
2. quantize the requested parcel through `HydroCoarseSeedGPU.plan_volume()`;
3. reserve that exact represented volume in the coarse store;
4. reserve a new sparse slot (`ALLOCATING`, occupancy still zero);
5. stage the real terrain bed into A+B;
6. seed the exact parcel into A+B;
7. commit the coarse debit;
8. activate/publish the sparse tile and rebuild connectivity.

The sparse runtime is paused for the short handoff and the bridge rejects a
request while the runtime is already busy. This first implementation also rejects
promotion into an already-live sparse tile; live-tile mutation must be integrated
at a solver idle/source boundary rather than writing ping-pong state concurrently.

If activation/connectivity fails after the coarse debit, the bridge restores the
same surface/channel categories through `accept_demotion()` and releases/unpublishes
the fine slot. Stale bytes in an unpublished/released atlas slot are not physical
ownership and are overwritten before reuse.

## Promotion footprint

A planetary macro cell can be kilometres wide while one sparse tile may only be
tens of metres wide. Therefore **never promote the entire coarse cell volume into
one fine tile**.

For flood/surface-water promotion, the bridge exposes:

```text
suggested_surface_volume =
    coarse_surface_depth * one_fine_tile_area
```

clamped to the coarse cell's available free-water volume. This transfers only the
fine footprint represented by the promoted tile. Channel-only promotion needs a
separate river/reach reconstruction policy and therefore requires an explicit
parcel volume for now.

## CPU conservation gate

Scene:

```text
tests/water/PlanetHydrologyStoreTests.tscn
```

Script:

```text
tests/water/test_planet_hydrology_store.gd
```

It uses the real `PlanetGrid` cube-sphere topology and `PlanetFields` constructor
on a tiny CPU-only world and checks:

- closed zero-forcing storage conservation;
- exact uniform-precipitation accounting;
- snapshot round-trip;
- prepare does not debit water;
- over-reservation rejection;
- coarse stepping blocked while ownership is unresolved;
- rollback preservation;
- exact commit debit;
- duplicate-commit rejection;
- snapshot fail-closed behavior during a pending transaction;
- restore clears transient reservation bookkeeping;
- fine-to-coarse demotion accounting;
- zero representation-aware mass error after a promotion/demotion round trip.

Run locally with the Godot 4.7 project build:

```text
godot --headless --path . tests/water/PlanetHydrologyStoreTests.tscn
```

The current ChatGPT environment does not contain the project Godot executable, so
this gate has not been runtime-executed here.

## Next integration gates

1. Runtime-run `PlanetHydrologyStoreTests.tscn` and fix parser/numerical issues if
   the project build exposes any; do not widen tolerances without evidence.
2. Add a renderer-mode sparse promotion smoke scene that creates one reserved tile,
   stages terrain, performs an exact seed, and verifies fine GPU volume against the
   committed coarse debit.
3. Expose the bridge from `WaterSystem` only after both `PersistentHydrologySystem`
   and the sparse runtime are ready.
4. Add a low-cadence promotion policy for surface/flood candidates. Keep channel
   promotion manual until a river/reach reconstruction model exists.
5. Add a representation-wide conservation diagnostic:

```text
coarse store volume + GPU-reduced active sparse volume + exported outlet volume
```

tracked across promotion/demotion cycles.
6. Replace uniform tile seeding with terrain-aware prolongation (equal free-surface
   reconstruction or conservative subcell distribution) while retaining the same
   exact-volume transaction/acknowledgement interface.
