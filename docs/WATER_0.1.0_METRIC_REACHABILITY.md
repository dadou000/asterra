# Water 0.1.0 — Metric addressing and frontier reachability

Branch: `water/0.1.0`

## Metric tile contract

Added:

```text
scripts/water/hydro_metric_grid.gd
```

A sparse SWE tile and its `HydroTileKey` must describe the same physical footprint.
For planet radius `R`, the nominal equi-angular cube-face arc is:

```text
face_arc = PI/2 * R
```

A level `L` quadtree tile therefore has nominal width:

```text
tile_width(L) = face_arc / 2^L
```

For fixed solver resolution `N`, the metric-compatible solver cell size is:

```text
dx(L,N) = tile_width(L) / N
```

`HydroMetricGrid` provides:

```text
level_for_target_cell_size(...)
compatible_cell_size_m(...)
contract_for_target(...)
atlas_contract(...)
```

The level is necessarily power-of-two quantized. Production bootstrap must choose
its desired level from a target resolution, then initialize `SparseHydroAtlasGPU`
with the returned **compatible** cell size. It must not initialize an arbitrary dx
and later pretend the nearest quadtree level has the same footprint.

`HydroSourceIngress` now derives its default point-source `tile_level` from this
contract using the same planet radius as `HydroTerrainBedGPU`. Explicit levels are
still accepted for isolated tests/special representations. If an externally-built
atlas has a >5% nominal footprint mismatch, ingress emits a diagnostic and exposes
the complete metric contract through `stats()` / `metric_contract()`.

The nominal metric is exact along the equi-angular face centreline. Local
cube-sphere metric distortion away from that centreline is a later refinement; the
important Phase 3 invariant is that address level and solver metres no longer vary
independently by arbitrary amounts.

## Compact source hydraulic head

`HydroTileActivityGPU` summary ABI is now five `vec4`s per resident slot:

```text
0  max depth, max velocity, kinetic proxy, invalid count
1  actual W/E/S/N outward discharge
2  wet count, ownership, reserved, reserved
3  predictive dry-neighbor W/E/S/N discharge
4  max free-surface eta=(bed+h) on W/E/S/N
```

The frontier queue snapshots the relevant edge `eta` into each candidate as:

```text
source_surface_m
```

No boundary-cell grid is read back. The queue entry grew from 32 to 48 bytes, still
small compared with one tile state, and stable tile identity remains included for
slot-recycle rejection.

`HydroFrontierResolver` preserves its existing five-argument reachability callback.
It enriches the topology-link dictionary with:

```text
source_surface_m
predictive_wetting
```

so old callbacks remain source-compatible while terrain-aware policy receives the
hydraulic head it needs.

## Terrain / structure reachability

Added:

```text
scripts/water/hydro_reachability_service.gd
```

The default terrain sampler is:

```text
GroundHeightStore.sample_height(direction, terrain_level)
```

therefore current sparse `TerrainDeltas` are included automatically.

For a candidate edge:

```text
GPU source edge max eta
        |
        v
exact cube-sphere destination edge
        |
        v
sample terrain crest at N points
        |
        +---- optional structure crest provider
        |       levee / dam / wall / gate / building
        v
minimum passable crest
        |
        v
eta - crest >= minimum_overtop_head ?
       yes -> frontier may reserve
       no  -> fail closed
```

The optional structure provider contract is:

```text
Variant(
    direction,
    source_key,
    destination_key,
    destination_direction,
    topology_link
)
```

A finite returned elevation raises the local crest. `null`, non-numeric or
non-finite means no structure contribution at that sample.

`evaluate()` returns diagnostics including:

```text
reachable
reason
source_surface_m
minimum_crest_m
overtopping_head_m
samples
crossed_face
```

while `can_enter()` is the bool callback used by `HydroFrontierResolver`.

### Why this is an allocation gate, not the final physics decision

The compact policy compares the highest source free-surface value on an edge with
the lowest sampled destination/structure crest. Those extrema can occur at
different edge positions, so the policy can occasionally allocate a neighbor that
ultimately receives no water.

That is acceptable and intentional: once the destination is resident, the normal
cell-level hydrostatic reconstruction/Rusanov interface uses the paired source and
destination cell beds and cannot transfer water through a locally higher barrier.
The reachability service is designed to avoid obviously impossible allocations,
not replace the conservative SWE interface.

A later optimization can carry several `(eta, edge_index)` samples per candidate if
allocation pressure proves high enough to justify a tighter CPU policy.

## Tests

Added:

```text
tests/water/HydroMetricGridTests.tscn
tests/water/test_hydro_metric_grid.gd

tests/water/HydroReachabilitySmoke.tscn
tests/water/test_hydro_reachability.gd
```

`HydroMetricGridTests` checks:

- exact level -> dx -> level roundtrips;
- nearest-level target selection;
- exact 2x metric refinement between adjacent quadtree levels.

`HydroReachabilitySmoke` uses the real GPU activity/frontier path with a stationary
2 m-deep source over a 10 m bed (`eta=12 m`). It verifies:

1. predictive wetting emits a candidate;
2. the candidate contains `source_surface_m ~= 12 m` even with runtime-style
   frontier initialization and no full-state readback;
3. an 11 m destination crest is accepted/reserved;
4. a 12.5 m terrain bank is blocked;
5. 11 m terrain plus a 13 m structure crest is blocked;
6. the resolver passes the hydraulic metadata through the unchanged policy
   callback signature.

Suggested runs:

```text
godot --headless --path . tests/water/HydroMetricGridTests.tscn
godot --path . tests/water/HydroReachabilitySmoke.tscn
```

Renderer execution is still pending in this ChatGPT environment because the
project's Godot 4.7 executable is unavailable here.

## Next integration

The sparse solver now has enough policy information for a safe production owner.
The next step is to bootstrap the sparse runtime from `WaterSystem` using one metric
configuration:

```text
target physical dx
      -> HydroMetricGrid contract
      -> exact compatible dx + address level
      -> SparseHydroAtlasGPU
      -> scheduler / identity / connectivity
      -> HydroReachabilityService
      -> SparseHydrologyRuntime
      -> source API exposed by WaterSystem
```

Production dynamic-surface rendering should remain opt-in until that owner is
validated in the normal world scene. After bootstrap, the next hydrology-specific
systems are distributed rain/runoff ingestion and terrain-edit invalidation.
