# Water 0.1.0 — Persistent sparse hydrology runtime

Branch: `water/0.1.0`

## Runtime controller

Added:

```text
scripts/water/sparse_hydrology_runtime.gd
```

`SparseHydrologyRuntime` converts the previously test-orchestrated Phase 3 pieces
into one persistent asynchronous state machine.

```text
queued physical time
        |
        v
SparseHydroStepGPU.advance()
        |
        | adaptive GPU CFL loop
        | may under-advance safely
        v
solver diagnostics
        |
        | subtract ONLY advanced_dt_s
        | retain unadvanced time debt
        v
HydroTileActivityGPU.classify()
        |
        +----> compact activity -> SparseHydroScheduler
        |
        v
HydroFrontierCandidatesGPU.generate()
        |
        v
HydroFrontierActivationPipeline
        |
        +----> reserve destination
        +----> GPU terrain reconstruction by default
        +----> conservative handoff
        +----> publish identity / occupancy
        +----> rebuild resident connectivity
        |
        v
next cycle with remaining time debt
```

The full h/hu/hv/bed atlas remains GPU-resident. CPU readback is currently limited
to compact solver diagnostics, per-slot activity summaries and the compact frontier
candidate queue because allocation, stable topology, settle/sleep policy and
terrain/structure reachability are still CPU policy decisions.

## CFL remainder handling

The runtime owns a bounded physical-time debt accumulator. A requested macro tick is
not assumed to have completed merely because one `SparseHydroStepGPU.advance()` call
returned.

For every cycle:

```text
requested = min(time_debt, macro_dt)
solver.advance(requested)
advanced = diagnostics.advanced_dt_s
time_debt -= advanced
```

If the GPU substep cap is reached, `advanced < requested`; the remainder remains in
`time_debt` and is submitted by a later cycle. The controller therefore never
forces an unsafe dt and never silently drops CFL-clamped simulation time.

`max_time_debt_s` bounds pathological catch-up after stalls. When no solver-visible
hydrology tile exists, automatic ticking clears time debt rather than accumulating a
large burst that would execute when the first water domain later appears.

## Stable slot lifecycle

Every runtime cycle preserves the existing two-stage allocation rule:

```text
reserve destination
    -> ALLOCATING
    -> CPU owns transient slot
    -> GPU occupancy remains 0

initialize destination terrain/state
    -> conservative handoff

activate_reserved
    -> ACTIVE
    -> identity + occupancy published
```

Activity summaries snapshot the stable tile ID associated with each slot before
settle/sleep policy is applied. Frontier generation is queued before any dry-tile
release/unbind work. A delayed candidate still passes through
`HydroFrontierResolver`, which verifies that its source slot continues to own the
same stable `HydroTileKey` before allocation is allowed.

After activity policy runs, resident connectivity is synchronized immediately. A
dry tile can therefore release/recycle its slot without leaving a stale neighbor
reference for the following SWE cycle.

## Wet freeze status

`SparseHydroScheduler.FROZEN_WATER` currently changes policy state but does not yet
remove the slot from GPU occupancy, so the solver would continue stepping it. The
runtime disables this placeholder freeze behavior by default.

True wet-domain freezing will be enabled only when the analytical/lumped-domain
collapse path exists and can conservatively remove a settled tile from the SWE
atlas while preserving its volume/state for later reconstruction.

## Destination initialization

The runtime accepts an explicit destination-state provider for numerical tests.
When none is supplied it creates `HydroTerrainBedGPU` and uses:

```text
HydroTileKey
    -> resident global macro terrain texture
    -> deterministic procedural detail
    -> sparse TerrainDeltas offset patch
    -> dry A/B bed state + cleared sources
    -> conservative handoff
```

Thus the default production path does not upload complete terrain tiles from CPU.

## Reachability

A reachability callable remains mandatory and fail-closed:

```text
bool(
    source_key,
    source_direction,
    destination_key,
    flux_m3s,
    topology_link
)
```

The runtime deliberately does not assume that topological adjacency means water can
enter a tile. Terrain banks, levees, player structures and other barriers must be
represented by this policy. A production terrain/structure reachability service is
the next integration item.

## Runtime smoke gate

Added:

```text
tests/water/SparseHydrologyRuntimeSmoke.tscn
tests/water/test_sparse_hydrology_runtime.gd
```

The gate deliberately configures `max_gpu_substeps = 1` for a 0.20 s requested
macro interval so the first solver call must hit a CFL cap. It verifies that:

1. the runtime exposes a nonzero remainder;
2. the remainder is carried into later cycles;
3. predictive frontier generation occurs without test-side orchestration;
4. one destination tile is reserved, initialized, conservatively pre-wet and
   activated;
5. source -> destination connectivity is published;
6. connected SWE continues while the remaining physical time is consumed;
7. occupied-tile water mass remains conserved;
8. deliberately stale water in an unoccupied atlas slot is never counted as
   physical state.

Suggested renderer-mode run:

```text
godot --path . tests/water/SparseHydrologyRuntimeSmoke.tscn
```

The current ChatGPT execution environment does not contain the project's Godot 4.7
renderer executable, so this scene is implemented but not runtime-passed here.

## Next integration

With persistent orchestration in place, the next useful Phase 3 work is the
production ingress side rather than another solver wrapper:

1. **Hydrology source injection** — rain/runoff, glacier melt, rivers, drains,
   gameplay emitters and reservoir/breach discharge must write conservative source
   terms into sparse tiles and wake/create the first domain when necessary.
2. **Terrain/structure reachability service** — replace test callbacks with a
   reusable boundary policy that understands terrain, banks, structures and edits.
3. **Terrain edit invalidation** — react to `Deltas.region_changed`, reconstruct
   affected active bed cells and reconcile water conservatively when ground moves.
4. **GPU Deltas mirror** — remove the remaining small CPU edit-offset patch.
5. **Physical LOD / analytical collapse** — conservative restriction,
   prolongation/refluxing and true wet-domain freeze/collapse.
