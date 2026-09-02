# Water 0.1.0 — Sparse 2D river -> persistent 1D collapse

Branch: `water/0.1.0`

## Scope

This milestone closes the explicit representation round trip for one promoted river
reach:

```text
persistent 1D reach
    -> transactional channel-only promotion
    -> terrain-following sparse 2D corridor
    -> continuous 1D <-> 2D mouth exchange
    -> quiet sparse-river collapse
    -> persistent 1D reach
```

Automatic channel promotion and automatic channel demotion remain OFF by default.

## Compact fine-state reduction

Added:

```text
shaders/water/sparse_hydro_tile_state_reduce.glsl
shaders/water/sparse_hydro_tile_state_finalize.glsl
scripts/water/sparse_hydro_tile_state_diagnostics_gpu.gd
```

One authoritative occupied sparse slot is reduced to 16 bytes:

```text
volume_m3
integral(hu dA)
integral(hv dA)
max_depth_m
```

The momentum integrals divided by volume produce a depth/volume-weighted mean local
velocity. Dry cells contribute no momentum, even if stale/recycled bytes exist.

The persistent 1D model remains volume-authoritative. Momentum is only a
reconstruction hint for the downstream velocity/discharge reported immediately after
collapse; subsequent 1D routing again follows the reach Manning geometry.

## Coupling suspension

`HydroRiverReachCouplingCollapse` adds a transactional suspension boundary to the
continuous mouth exchange controller.

```text
registered refined reach
        |
        v
suspend_reach(cell)
        |
        +-- removed from future GPU mouth exchanges
        +-- sparse tile remains authoritative
        +-- coarse refinement record remains authoritative
        +-- pending donor/confluence inflow remains coarse-owned
```

A collapse attempt can then either:

```text
rollback
  -> resume_suspended_reach(cell)

success
  -> finalize_suspended_reach(cell)
  -> pending coarse inflow folds back into channel_storage_m3
  -> refinement hole disappears
```

## Transactional collapse bridge

Added:

```text
scripts/water/planet_river_reach_collapse_bridge.gd
scripts/water/planet_river_reach_collapse_bridge_production.gd
```

Normal ordering:

```text
quiet refined tile
    -> suspend continuous coupling
    -> pause sparse + coarse advancement
    -> GPU reduce exact fine volume/momentum
    -> prepare channel-only incoming coarse demotion
    -> unpublish fine tile
    -> rebuild sparse connectivity
    -> commit fine volume into 1D channel storage
    -> return still-coarse pending confluence inflow
    -> remove refined-reach metadata
    -> reconstruct 1D depth/stage + discharge hint
    -> resume pure 1D routing
```

Every positive measured fine volume is transferred transactionally. Exact zero may
collapse without creating an incoming volume transaction, while pending coarse inflow
is still returned to normal channel storage.

## Quiet eligibility

The production bridge uses `HydroRiverCollapsePolicy`.

Default requirements:

```text
state                    SETTLING or FROZEN_WATER
quiet_time_s             >= 20 s
max_velocity_mps         <= 0.01
max_outgoing_flux_m3s    <= 0.01
max_disturbance_energy   <= 5e-5
```

Eligibility also requires:

- a live continuous-coupling registration;
- exact stable tile/slot identity;
- no pending mouth exchange;
- no coarse ownership transaction;
- sparse runtime idle.

The policy is independent of camera distance.

## Failure semantics

Before fine unpublish:

```text
failure
  -> rollback incoming coarse reservation
  -> resume mouth coupling
  -> fine remains authoritative
```

After fine unpublish:

```text
late failure
  -> try to reacquire the exact just-freed slot
  -> republish existing raw fine A/B state
  -> rollback coarse reservation
  -> resume coupling
```

If exact fine restoration cannot be guaranteed:

```text
commit validated fine parcel to coarse channel
  -> finalize refinement hole
  -> return pending coarse inflow
  -> keep failed operation contained without deleting water
```

If refinement finalization itself becomes inconsistent after coarse ownership is
already guaranteed, both simulation owners stay paused for diagnosis rather than
continuing through ambiguous topology.

The production wrapper also defers RID teardown while the 16-byte asynchronous GPU
readback is pending. A retiring sparse generation is never restarted after the
transaction finishes.

## WaterSystem API

Production `WaterSystem` now exposes:

```gdscript
river_reach_collapse_bridge_available()
river_reach_collapse_eligible(cell)
collapse_fine_river_reach(cell, ignore_quiet_policy = false)
```

A generic surface demotion is explicitly rejected for a cell currently registered as
a refined river. Refined river water must return through the channel-aware collapse
path so continuous-coupling metadata and channel ownership cannot be bypassed.

Switch installed but OFF:

```gdscript
WaterSystem.automatic_channel_demotion_enabled = false
```

## Validation gates

CPU/headless quiet policy:

```text
tests/water/HydroRiverCollapsePolicyTests.tscn
```

Compact GPU state reducer:

```text
tests/water/SparseHydroTileStateDiagnosticsGPU.tscn
```

Full renderer-mode round trip:

```text
tests/water/PlanetRiverReachCollapseBridgeTests.tscn
```

The round-trip gate performs:

```text
initial 1D channel
    -> real corridor promotion
    -> continuous coupling registration
    -> internal pending confluence parcel
    -> quiet scheduler state
    -> real compact GPU collapse reduction
    -> 1D channel return
```

and requires:

```text
channel_final == channel_initial
surface_final == surface_initial
coarse_total_final == coarse_total_initial
fine_authoritative_final == 0
refined_reach_count == 0
pending confluence parcel returned exactly
cumulative_promoted_to_fine == cumulative_demoted_from_fine
mass_error == 0
```

## Production status

The implementation exists on `water/0.1.0`, but the current ChatGPT execution
environment does not contain the project's Godot 4.7 executable. These gates are not
runtime-passed here.

Automatic channel promotion/demotion should remain disabled until the river promotion,
continuous coupling, compact reduction and collapse gates have all run successfully on
the project renderer/GPU.
