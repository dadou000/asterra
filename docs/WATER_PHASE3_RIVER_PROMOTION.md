# Water Phase 3 — 1D River Reach → Sparse 2D Promotion

Branch: `water/0.1.0`

## Scope

This checkpoint adds the first conservative local refinement path from the persistent
1D macro-river representation into sparse 2D SWE.

It does **not** yet enable automatic channel promotion. Continuous coarse/fine
through-flow, confluence exchange and symmetric 2D→1D river collapse remain the
next river-specific layer.

## Channel-only ownership

`PlanetHydrologyRiverPromotionStore` adds an explicit channel reservation:

```text
prepare_channel_promotion(cell, V)
```

The transaction reserves:

```text
surface = 0
channel = V
```

and uses the existing ownership ledger's `commit_promotion()` /
`rollback_promotion()` implementation. A river promotion therefore cannot silently
borrow floodplain/sheet storage even when large surface storage exists in the same
coarse cell.

The default local parcel is the current 1D cross-sectional area multiplied by at
most one fine-tile span:

```text
Vlocal = A_channel(current depth) × min(fine tile span, coarse reach length)
```

capped by currently free channel storage. One local refinement never dumps an
entire kilometre-scale macro reach into one small sparse tile.

## Terrain-aligned corridor reconstruction

Added:

```text
shaders/water/hydro_river_corridor_prolongation.glsl
scripts/water/hydro_river_corridor_prolongation_gpu.gd
```

The fine tile remains unpublished while terrain is staged. The coarse reach centre
is mapped into the exact HydroTileKey cell coordinate. The generated receiver
provides a world-space downstream tangent, which is projected into the same local
face-u/v basis used by sparse SWE momentum.

The corridor uses the generated river width, with a minimum width large enough to
cover at least one fine cell. Only cells whose centres lie inside this corridor are
eligible for river water.

Within the corridor the GPU solves:

```text
Σ max(η - bed_i, 0) × cell_area = target channel parcel
```

using relative bed elevations and fixed bisection. High terrain inside the nominal
corridor can therefore remain dry; terrain is not overwritten to force a uniform
river depth. Outside the corridor h/hu/hv remain zero.

Momentum is initialized as:

```text
hu = h × u_downstream
hv = h × v_downstream
```

where downstream speed is derived from the current 1D Manning cross-section.

A and B receive identical reconstructed state before the slot is published.

## Transaction ordering

`PlanetRiverReachPromotionBridge` performs:

```text
1D channel reach
    ↓
plan exact non-increasing FP32 parcel
    ↓
prepare CHANNEL-only ownership transaction
    ↓
reserve sparse slot (ALLOCATING, occupancy=0)
    ↓
GPU terrain stage
    ↓
GPU terrain-aware river corridor prolongation
    ↓
seed acknowledgement
    ↓
commit coarse channel debit
    ↓
activate sparse tile
    ↓
rebuild connectivity
```

If publication/connectivity fails after the channel debit, the bridge returns the
same parcel to coarse **channel** storage rather than surface storage.

## Production API

`WaterSystem` now uses the river-aware facade and exposes:

```text
river_reach_promotion_bridge_available()
river_reach_promotion_bridge_state()
channel_reach_candidates(...)
suggested_river_reach_promotion_volume_m3(cell)
promote_coarse_river_reach(cell, requested_volume_m3=-1)
```

`automatic_channel_promotion_enabled` remains `false`.

Surface flood promotion/demotion and river promotion are single-flight at the
production facade so their ownership transactions cannot overlap.

## Gates

CPU/headless category ownership:

```text
godot --headless --path . tests/water/PlanetRiverChannelOwnershipTests.tscn
```

Checks that river promotion reserves/debits channel storage only and round-trips
through channel demotion without touching surface storage.

GPU corridor reconstruction:

```text
godot --path . tests/water/HydroRiverCorridorProlongationGPU.tscn
```

Checks exact volume, A/B identity, dry off-corridor cells, high terrain staying dry,
level wet stage and downstream momentum.

End-to-end bridge ownership:

```text
godot --path . tests/water/PlanetRiverReachPromotionBridgeTests.tscn
```

Checks channel-only coarse debit and:

```text
coarse_after + occupied fine GPU volume ≈ coarse_before
```

## Remaining river-specific work

The promoted tile currently owns a correctly reconstructed local river parcel, but
normal 1D routing is not yet replaced by live exchange boundary conditions through
that fine segment. Therefore automatic channel promotion remains disabled.

Next river work:

1. register promoted reach segments and their 1D ownership holes;
2. inject upstream 1D discharge into the fine corridor boundary conservatively;
3. return measured downstream fine discharge to the 1D receiver;
4. handle tributary/confluence fan-in without double routing;
5. preserve coarse travel time outside the refined segment;
6. implement symmetric quiet fine-river → 1D reach collapse;
7. only then enable low-cadence automatic channel promotion.

The current ChatGPT environment does not contain the project's Godot 4.7 executable,
so these gates are implemented but not runtime-passed here.
