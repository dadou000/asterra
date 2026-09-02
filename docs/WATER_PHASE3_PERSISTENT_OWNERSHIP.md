# Water Phase 3 — persistent hydrology ownership

Branch: `water/0.1.0`

## Purpose

The planetary coarse hydrology store and sparse fine SWE are different physical
representations of the same water. Promotion and collapse are therefore ownership
transfers, not source/sink terms.

The central invariant is:

```text
one physical parcel -> one authoritative representation
```

A parcel may be in coarse storage, in fine sparse SWE, or in a transaction that
locks both simulation sides while ownership changes, but it must never be simulated
by both or silently disappear from both.

## Bidirectional transaction model

`PlanetHydrologyOwnershipStore` extends `PlanetHydrologyStore` without changing the
base routing equations.

### Coarse -> fine

```text
prepare_promotion(volume)
        |
        | reserve only; coarse physical storage unchanged
        v
reserve hidden sparse slot + stage terrain
        |
        v
exact one-shot GPU seed into hidden A+B
        |
        | GPU seed recorded
        v
commit_promotion()
        |
        | exact coarse debit
        v
activate_reserved() + publish sparse identity/connectivity
```

Failure before coarse commit calls `rollback_promotion()`. A late
activation/connectivity failure after commit restores the exact coarse categories
before the fine slot is abandoned.

### Fine -> coarse

```text
compact GPU reduce of one canonical occupied tile
        |
        | exact measured fine volume
        v
prepare_demotion(volume)
        |
        | validate/lock only; coarse storage unchanged
        v
unpublish/release fine tile
        |
        v
rebuild sparse connectivity
        |
        v
commit_demotion()
        |
        | exact coarse addition
        v
resume sparse/coarse simulation
```

`rollback_demotion()` is valid only while fine remains authoritative. Once fine has
been unpublished, rollback would delete the parcel; late failure recovery must
instead restore the same fine publication or commit the already-validated incoming
coarse transaction.

While either transaction direction is unresolved, coarse `step()` returns
`ERR_BUSY`, and `snapshot()` fails closed. Promotion and demotion transactions are
mutually exclusive in the ownership store and in the production `WaterSystem`
facade.

## Representation-aware ledger

Environmental balance remains separate from representation transfers:

```text
expected coarse storage =
    initial coarse storage
  + precipitation
  + climatology input
  + demoted from fine
  - outlet export
  - promoted to fine
```

The ownership store tracks:

```text
cumulative_promoted_to_fine_m3
cumulative_demoted_from_fine_m3
```

Promotion/demotion therefore never masquerade as rainfall, evaporation or outlet
loss.

## Exact coarse -> fine seed

`HydroCoarseSeedGPU` + `hydro_coarse_seed.glsl` operate only on an unpublished
sparse atlas slot whose terrain has already been staged.

The seed is written identically to A and B:

```text
h  += dh
hu += dh * u
hv += dh * v
bed unchanged
```

`plan_volume()` quantizes `dh` to the actual FP32 value sent to the GPU and reports
the resulting represented volume. The coarse ownership transaction reserves and
debits this represented volume, preventing systematic CPU/GPU mismatch.

The initial spatial reconstruction is uniform over one tile. It is conservative but
not intended as the final equilibrium reconstruction. Terrain-aware prolongation can
replace it later without changing the exact-volume acknowledgement contract.

## Compact fine -> coarse reduction

`SparseHydroTileVolumeDiagnosticsGPU` +
`sparse_hydro_tile_mass_reduce.glsl` reduce one requested occupied atlas slot from
canonical state A to a single FP32 volume value.

Only four bytes are asynchronously read back. Full `h/hu/hv/bed` state never leaves
the GPU for production collapse.

The default demotion bridge treats only an **exact zero** measurement as dry.
Positive measured water, however small, enters a real incoming coarse transaction by
default.

## Production bridges

### `PlanetHydroPromotionBridge`

1. resolve target `HydroTileKey`;
2. quantize requested parcel;
3. reserve exact represented coarse volume;
4. reserve new sparse slot (`ALLOCATING`, occupancy zero);
5. stage bed into A+B;
6. seed exact parcel into A+B;
7. commit coarse debit;
8. activate/publish tile;
9. sync connectivity.

Promotion into an already-live tile remains rejected. Live-tile additions belong at
a solver idle/source boundary.

### `PlanetHydroDemotionBridge`

1. verify resident non-allocating fine tile;
2. pause sparse runtime;
3. reduce exact occupied tile volume from canonical A;
4. prepare incoming coarse surface parcel;
5. unpublish/release fine tile;
6. sync connectivity;
7. commit incoming coarse parcel;
8. resume runtime.

The current reverse classification returns the whole collapsed parcel to coarse
**surface** storage. Channel classification is deferred until the persistent 1D
river/reach representation exists.

Late failure handling:

```text
fine still published
    -> rollback incoming coarse reservation
    -> fine remains authoritative

fine already unpublished
    -> try to reacquire the same just-freed slot and republish raw intact state
    -> if republish succeeds: rollback incoming coarse reservation
    -> if republish fails: commit validated incoming coarse fallback
```

The scheduler is LIFO, and the runtime is paused during this recovery, so the
just-freed slot is expected to be the next reservation. If that invariant does not
hold, the bridge chooses the coarse fallback rather than losing the parcel.

## Promotion footprint

A coarse planetary cell may be kilometres wide while one sparse tile may be tens of
metres wide. Never dump the full coarse-cell volume into one fine tile.

Surface/flood promotion uses:

```text
suggested_surface_volume =
    coarse_surface_depth * one_fine_tile_area
```

clamped to available free coarse water. Channel-only promotion requires a separate
reach-aware policy.

## Spatial precipitation authority

`HydroCoarseFineOwnershipMap` assigns every solver-visible fine tile footprint to
the containing coarse macro cell and subtracts that area from coarse distributed
native precipitation.

```text
coarse precipitation fraction =
    1 - fine_owned_area / coarse_cell_area
```

The map listens to sparse tile publication/release, including cross-face topology.
`HydroWeatherCoupling` claims fine precipitation authority only after fine forcing
is actually being published and returns authority to coarse before fine forcing is
cleared.

A successful fine->coarse demotion emits the normal sparse release lifecycle, so
rainfall authority returns to coarse automatically with no second mapping system.

## Frontier ownership

A frontier-created destination remains hidden until terrain and ownership handoff
are complete.

Failure classification is:

```text
0 successful fine->fine edge handoffs
    -> any coarse preseed is reversible
    -> restore coarse parcel
    -> release hidden destination

1+ successful fine->fine edge handoffs
    -> source fine state has already been debited
    -> destination owns irreversible fine water
    -> preserve/publish destination
```

A late failure is never allowed to cancel a destination that already received
fine->fine water.

## Failed sparse generations

A sparse runtime failure is no longer treated as permission to delete physical
state.

If the failed scheduler still has allocated fine tiles, `WaterSystem` preserves the
failed atlas generation in place instead of automatically releasing it. An in-flight
fine->coarse transaction is also allowed to finish before failed-generation teardown
is considered.

Only an empty failed generation is automatically torn down.

This is a containment rule, not the final recovery system. A later recovery/save
path should explicitly collapse or serialize fine ownership before replacing a
failed or transitioning world generation.

## Automatic surface policies

Both production switches remain off by default pending local runtime gates:

```gdscript
WaterSystem.automatic_coarse_promotion_enabled = false
WaterSystem.automatic_fine_demotion_enabled = false
```

### Automatic promotion

`HydroAutomaticSurfacePromotion`:

- low cadence;
- surface-depth-only eligibility;
- 5 cm enter / 2.5 cm exit hysteresis;
- never borrows channel storage;
- one transaction per scan;
- existing sparse ownership suppresses duplicate promotion;
- successful policy-owned tiles are explicitly registered for possible automatic
  collapse.

### Automatic demotion

`HydroAutomaticSurfaceDemotion` only considers tiles registered by the automatic
promotion policy.

Initial eligibility is intentionally conservative:

```text
state               = SETTLING or FROZEN_WATER
quiet_time           >= 20 s
max_depth            <= 0.15 m
max_velocity         <= 0.004 m/s
max_outgoing_flux    <= 0.002 m3/s
disturbance_energy   <= 2.5e-5
resident cardinal neighbors = none
```

No manual, point-source, frontier-created or connected fine domain is automatically
collapsed by this first policy. Connected-component collapse is a separate future
transaction problem.

## Production facade

`WaterSystem` exposes:

```text
planet_promotion_bridge_available()
planet_promotion_bridge_state()
planet_promotion_bridge()
coarse_promotion_candidates(...)
suggested_surface_promotion_volume_m3(cell)
promote_coarse_surface_cell(cell, volume=-1, local_velocity=Vector2.ZERO)

planet_demotion_bridge_available()
planet_demotion_bridge_state()
planet_demotion_bridge()
demote_fine_surface_cell(cell)
```

Promotion and demotion calls refuse to overlap.

## Validation gates

CPU/headless:

```text
tests/water/PlanetHydrologyStoreTests.tscn
tests/water/PlanetHydroDemotionTransactions.tscn
tests/water/HydroFrontierFailurePolicyTests.tscn
tests/water/HydroPrecipitationOwnershipTests.tscn
tests/water/HydroAutomaticSurfacePromotionTests.tscn
tests/water/HydroAutomaticSurfaceDemotionTests.tscn
```

Renderer-mode:

```text
tests/water/HydroCoarseSeedGPUSmoke.tscn
tests/water/PlanetHydroPromotionBridgeTests.tscn
tests/water/HydroAutomaticSurfacePromotionGPU.tscn
tests/water/PlanetHydroDemotionBridgeTests.tscn
tests/water/HydroAutomaticSurfaceDemotionGPU.tscn
tests/water/HydroRepresentationAuditTests.tscn
tests/water/SparseHydroFrontierHandoff.tscn
```

The bidirectional renderer gate checks:

```text
coarse_initial
    -> exact fine seed
    -> compact fine tile reduction
    -> transactional fine unpublish
    -> exact coarse return

coarse_final == coarse_initial
active_fine_final == 0
promoted_ledger == demoted_ledger
mass_error == 0
```

The automatic-demotion GPU gate performs the same physical round trip through the
actual quiet-surface policy selection path.

The current ChatGPT environment does not contain the project Godot 4.7 executable,
so none of these newly added gates have been runtime-executed here.

## Remaining ownership gates

1. Run all bidirectional CPU/GPU ownership tests on the project Godot 4.7 build and
   fix real parser/API/numerical failures without speculative tolerance widening.
2. Add cumulative fine external-source ledgers for atmospheric precipitation and
   gameplay/world sources so representation-wide conservation audits close during
   normal forced simulation.
3. Add connected-component/cluster fine->coarse collapse so quiet multi-tile flood
   regions can be reduced without punching holes in a live sparse domain.
4. Replace uniform promotion seeding with terrain-aware conservative prolongation.
5. Add persistent 1D river/reach ownership before channel-only automatic promotion
   or collapse is allowed.
6. Add explicit save/world-transition ownership flushing/serialization for fine
   state before replacing a world generation.
