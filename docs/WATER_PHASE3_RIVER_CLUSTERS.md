# Water Phase 3 — Multi-tile river refinement clusters

Branch: `water/0.1.0`

## Purpose

A promoted 1D macro reach no longer has to become one isolated sparse tile. It may
now be represented by a short, contiguous chain of sparse SWE tiles:

```text
persistent upstream 1D
        |
        | external 1D -> 2D mouth
        v
   [ fine member 0 ]
        <->
   [ fine member 1 ]
        <->
   [ fine member 2 ]
        |
        | external 2D -> 1D mouth
        v
persistent residual 1D
```

Internal arrows are ordinary connected sparse SWE interfaces. They are not coarse
ownership transfers and do not enter the environmental source ledger.

This first cluster milestone deliberately represents **one coarse 1D reach with a
short fine chain**. Joining multiple macro reaches/confluences into one larger fine
cluster is a later extension built on the same registry.

## Coarse ownership

`PlanetHydrologyRiverClusterStore` extends the existing coupled river store.

A cluster remains exactly one coarse refinement hole:

```text
cell -> {
    represented_volume_m3,
    represented_length_m,
    residual_length_m,
    members: [member0, member1, ...]
}
```

There is no coarse record per sparse member and no duplicated channel water.

The first member remains the legacy `tile_id` / `slot` identity for existing debug
interfaces, while cluster-aware code uses the ordered `members` array.

Pending tributary/confluence inflow remains one coarse-owned queue for the reach.

## Cluster planning

`HydroRiverClusterPlanner`:

1. resolves the exact hydrology quadtree level from the atlas metric contract;
2. starts at the macro reach's planet-space direction;
3. transforms downstream direction into each current cube-face frame;
4. follows the dominant cardinal `HydroTileTopology` neighbor;
5. repeats up to the requested cluster size / reach length;
6. records exact pairwise topology, including cube-face seam orientation.

Using cardinal sparse neighbors is intentional. The existing sparse connectivity
system then opens each fine/fine interface without a second river-specific topology.

Default production request: **3 sparse members**.

## Transactional promotion

`PlanetRiverReachClusterPromotionBridge` performs one transaction for all members:

```text
plan contiguous chain
        |
quantize each member parcel to representable FP32
        |
prepare ONE channel promotion for sum(member volumes)
        |
reserve every sparse member as ALLOCATING
        |
stage terrain for every member
        |
seed terrain-following corridor in every member
        |
commit ONE coarse channel debit
        |
batch-publish all members
        |
rebuild sparse connectivity once
```

No member becomes solver-visible before every member has a valid A/B state.

`HydroSchedulerBatchOps` prevalidates all members before pool state changes and
emits wake/release identity signals only after the whole pool batch has changed.

## Continuous coupling

The existing river exchange ABI now has per-record mouth flags:

```text
bit 0: upstream 1D -> 2D mouth enabled
bit 1: downstream 2D -> 1D mouth enabled
```

For one member, both flags remain enabled (backward compatible).

For a cluster:

```text
member 0      upstream=true   downstream=false
middle        upstream=false  downstream=false
member N-1    upstream=false  downstream=true
```

`HydroRiverReachClusterCoupling` submits only the first and last boundary records.
It aggregates their results by coarse cell before changing ownership ledgers:

- first-member actual addition consumes pending coarse inflow;
- last-member actual removal credits residual coarse channel storage;
- last-member measured Q becomes the reach's hybrid downstream Q.

Internal member boundaries are pure sparse SWE fluxes.

## Atomic collapse

`PlanetRiverReachClusterCollapseBridge` requires every member to satisfy the normal
quiet-river collapse policy.

```text
suspend reach coupling
        |
pause coarse + sparse owners
        |
GPU-reduce member 0 (16 B)
GPU-reduce member 1 (16 B)
...
        |
aggregate exact cluster volume
        |
prepare ONE channel demotion
        |
batch-unpublish all members
        |
rebuild connectivity once
        |
commit ONE coarse channel return
        |
return pending coarse inflow
        |
remove refinement hole
```

Only the downstream/final member's volume-weighted local velocity is used as the
immediate 1D discharge reconstruction hint.

### Late-failure recovery

`HydroTilePool` appends released slots and allocates from the end. Cluster batch
release occurs in member order. Therefore reserving the members in reverse order
can recover the exact previous slots.

If connectivity or the coarse commit fails after fine unpublication:

1. reserve members in reverse order;
2. require exact old slots;
3. batch-reactivate the cluster;
4. rebuild connectivity;
5. rollback coarse incoming ownership;
6. resume fine coupling.

If exact fine restoration cannot be guaranteed, the already measured aggregate
parcel is completed on the coarse side and the system fails closed rather than
losing water.

## Automatic refinement interaction

`WaterSystem.promote_coarse_river_reach()` now prefers a cluster when the cluster
bridge is ready. The existing automatic channel policy calls that same public API,
so no policy fork is required.

Production still keeps:

```gdscript
WaterSystem.automatic_channel_promotion_enabled = false
WaterSystem.automatic_channel_demotion_enabled = false
```

The WaterSystem cluster facade also checks sparse member capacity before submitting
a default cluster. This prevents a coarse reach-count limit from overcommitting an
atlas when one refinement now consumes several slots.

## Validation gates

CPU coarse ownership:

```text
godot --headless --path . tests/water/PlanetRiverClusterStoreTests.tscn
```

Boundary mouth ABI:

```text
godot --path . tests/water/HydroRiverClusterBoundaryExchangeGPU.tscn
```

Full cluster promotion/collapse:

```text
godot --path . tests/water/PlanetRiverClusterRoundTrip.tscn
```

The full renderer gate requires:

- exactly three resident cluster members;
- both internal member links published by sparse connectivity;
- one coarse refinement hole;
- coarse + represented fine volume conservation after promotion;
- all members quiet before collapse;
- aggregate collapsed volume equals promoted cluster volume;
- all sparse members removed atomically;
- channel storage and total coarse water return to baseline;
- promotion/demotion ledgers close;
- final authoritative sparse volume equals zero.

These scenes are implemented but have not been runtime-executed in the current
assistant environment because the project Godot 4.7 renderer executable is absent.

## Next extension

The cluster registry now provides the base for **cross-macro-reach cluster merging**:

```text
tributary 1D ----\
                  > [shared fine confluence cluster] -> fine main stem -> 1D
main 1D ----------/
```

That later step needs:

- cluster union/merge transactions;
- multiple external upstream mouths;
- one downstream mouth per connected fine component;
- fine confluence topology and tributary priority;
- component-level collapse rather than one coarse-reach collapse.
