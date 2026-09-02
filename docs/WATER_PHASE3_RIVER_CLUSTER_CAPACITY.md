# Water 0.1.0 Phase 3 — River Cluster Capacity Accounting

## Purpose

Multi-tile river refinement changes the resource unit from one coarse reach to one or
more sparse SWE members. A limit expressed only as `refined_reach_count()` is no
longer sufficient: eight refined reaches can occupy eight slots, twenty-four slots,
or another amount depending on cluster length.

The production automatic channel policy therefore enforces capacity in **fine member
slots** before asking any ownership bridge to run.

## Two independent capacity guards

### 1. River refinement member budget

`HydroAutomaticChannelRefinementClusterCapacity` keeps:

```gdscript
max_auto_refined_member_slots = 24
```

The current value preserves the previous default scale of eight automatic reaches
with three-member clusters, while still allowing shorter reaches to consume fewer
slots.

The used count comes from:

```gdscript
PlanetHydrologyRiverClusterStore.refined_sparse_member_count()
```

This count includes every registered river refinement, including manually promoted
and legacy one-tile reaches. Automatic policy therefore cannot pretend manually
refined rivers are free.

The counter is O(number of refined reaches). It deliberately does not call the
full store `stats()` path, because the parent hydrology statistics currently scan
planetary coarse cells for other aggregates.

### 2. Real sparse scheduler capacity

The policy also queries:

```gdscript
WaterSystem.river_cluster_free_member_slots()
```

which resolves to `SparseHydroScheduler.pool.free_count()`.

This is a separate constraint from the river budget. Flood, frontier, gameplay, or
other sparse domains consume the same atlas, so a river cluster may be under its
river-specific budget and still be rejected because the physical sparse pool lacks
all required slots.

## Exact planned member count

Before promotion, WaterSystem evaluates:

```gdscript
river_cluster_requested_member_count(cell)
```

through `HydroRiverClusterPlanner.plan(...)`.

The result is the exact member count the current reach would request. A configured
three-member cluster can therefore request one or two members for a short reach
instead of being charged three unconditionally.

The capacity requirement is:

```text
used_refined_river_members + requested_members <= river_member_budget

AND

sparse_pool_free_slots >= requested_members
```

Both must be true.

## Race closure

Capacity is checked twice:

```text
automatic policy scan
    -> exact planned member count
    -> river-member budget check
    -> current sparse free-slot check
    -> WaterSystem.promote_coarse_river_reach()
        -> re-plan same reach
        -> re-check sparse free slots
        -> PlanetRiverReachClusterPromotionBridge.promote_cluster()
            -> reserve all members synchronously
            -> only then stage/seed GPU state
```

The second check closes the interval between low-cadence policy selection and the
actual ownership request. The cluster bridge then reserves the complete member set
before any asynchronous GPU work. If the complete set cannot be reserved, coarse
ownership is not debited and no partial fine cluster is published.

## Diagnostics

`HydroChannelRefinement.stats()` now reports:

```text
max_auto_refined_member_slots
refined_river_member_slots
last_requested_member_slots
free_sparse_slots
member_budget_suppressed
sparse_slot_suppressed
cluster_capacity_guard = true
counts_manual_refinements = true
```

These are policy diagnostics only; the authoritative physical capacity remains the
sparse scheduler pool.

## Validation gates

Pure capacity/hysteresis policy:

```text
godot --headless --path . tests/water/HydroAutomaticChannelRefinementTests.tscn
```

Production-policy integration with real coarse river candidates:

```text
godot --headless --path . tests/water/HydroAutomaticChannelCapacityTests.tscn
```

The integration gate requires:

- a three-member request with only two sparse slots free never calls promotion;
- a three-member request exceeding the configured river-member budget never calls
  promotion;
- an exact three-slot / three-member fit enters exactly one single-flight promotion.

Cluster-store member accounting:

```text
godot --headless --path . tests/water/PlanetRiverClusterStoreTests.tscn
```

It verifies a three-member cluster reports three refined sparse members and returns
to zero after unregister/collapse-side ownership cleanup.

## Production state

This capacity work does **not** enable automatic river refinement. Production still
keeps:

```gdscript
WaterSystem.automatic_channel_promotion_enabled = false
WaterSystem.automatic_channel_demotion_enabled = false
```

The new guards are active infrastructure for the point at which those switches are
explicitly enabled after local Godot/GPU validation.
