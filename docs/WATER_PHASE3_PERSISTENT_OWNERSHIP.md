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

## WaterSystem production facade

The original 0.1.0 water coordinator is preserved byte-for-byte as:

```text
scripts/water/water_system_base.gd
```

The active autoload remains:

```text
scripts/water/water_system.gd
```

but is now a thin derived facade that owns promotion binding/lifecycle. This keeps
all pre-existing sparse runtime, render-cache and point-source behavior in the base
script while making the ownership extension isolated and reversible.

Bridge readiness is event-driven from both sides:

```text
PersistentHydrologySystem.store_rebuilt
            +
WaterSystem sparse runtime READY
            |
            v
PlanetHydroPromotionBridge initialize
            |
            v
planet_promotion_bridge_ready
```

A coarse-store rebuild releases the prior bridge before rebinding. Any sparse
runtime transition away from READY also releases it, preventing references to
recycled atlas/connectivity/terrain resources. `_release_sparse_runtime()` is
additionally overridden so an in-flight ownership transaction is rolled back while
its coarse store and sparse scheduler are still valid.

Production/manual API:

```text
WaterSystem.planet_promotion_bridge_available()
WaterSystem.planet_promotion_bridge_state()
WaterSystem.coarse_promotion_candidates(...)
WaterSystem.suggested_surface_promotion_volume_m3(cell)
WaterSystem.promote_coarse_surface_cell(cell, volume=-1, local_velocity=Vector2.ZERO)
```

`volume < 0` selects the flood-oriented suggested footprint parcel.

Automatic policy is intentionally still disabled:

```text
WaterSystem.automatic_coarse_promotion_enabled == false
```

No background candidate loop is installed yet. Explicit promotion is available
for validation/gameplay tooling without silently changing production water
ownership policy.

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

## Exact seed GPU gate

Scene:

```text
tests/water/HydroCoarseSeedGPUSmoke.tscn
```

This renderer-mode test seeds a known parcel into a one-slot sparse atlas and
uses independent GPU volume reducers on state A and state B. Both ping-pong states
must contain the exact FP32-represented parcel before the gate passes.

```text
godot --path . tests/water/HydroCoarseSeedGPUSmoke.tscn
```

## End-to-end promotion conservation gate

Scene:

```text
tests/water/PlanetHydroPromotionBridgeTests.tscn
```

Script:

```text
tests/water/test_planet_hydro_promotion_bridge.gd
```

This gate uses:

- the real `PlanetHydrologyOwnershipStore`;
- real `SparseHydroScheduler`;
- real `SparseHydroAtlasGPU`;
- real `SparseHydroIdentityBridge`;
- real `SparseHydroConnectivityGPU`;
- real `PlanetHydroPromotionBridge`;
- real `HydroCoarseSeedGPU`;
- independent GPU volume reduction of both A and B.

Only terrain reconstruction is stubbed with a deterministic flat dry bed so the
numerical ownership gate does not depend on a full procedural planet bake.

After promotion it checks the authoritative conservation identity:

```text
coarse_after + fine_GPU_volume == coarse_before
```

It also checks that the coarse ownership ledger is closed, both GPU ping-pong
states contain the acknowledged parcel, the sparse tile is resident only after
acknowledgement, and no coarse transaction remains pending.

The final test step unpublishes the fine tile and returns the exact acknowledged
parcel through `accept_demotion()`, checking that coarse storage and the transfer
ledger return to the original state.

```text
godot --path . tests/water/PlanetHydroPromotionBridgeTests.tscn
```

The current ChatGPT environment does not contain the project Godot executable, so
none of these newly added ownership gates have been runtime-executed here.

## Remaining integration gates

1. Runtime-run all three ownership tests above on the project Godot 4.7 build and
   fix parser/API/numerical issues; do not widen conservation tolerances without
   measured evidence.
2. Add a GPU reduction over **all occupied sparse tiles**, rather than one fixed
   tile, to expose representation-wide active fine-water volume without full-state
   readback.
3. Track the global diagnostic:

```text
coarse store volume
+ active sparse GPU volume
+ water already exported through outlets
```

across explicit promotion/demotion cycles and sparse frontier expansion.
4. Once that accounting gate is stable, add a low-cadence automatic policy for
   **surface/flood** candidates only. Keep channel/river promotion manual until a
   reach-aware reconstruction model exists.
5. Replace uniform tile seeding with terrain-aware prolongation (equal free-surface
   reconstruction or conservative subcell distribution) while retaining the same
   exact-volume transaction/acknowledgement interface.
6. Implement true fine -> coarse collapse: GPU-reduce an inactive wet tile/reach,
   atomically unpublish it, then return the exact reduced parcel to the coarse
   store. Do not use raw stale atlas bytes as ownership after release.
