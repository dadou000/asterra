# Water 0.1.0 — Phase 3 sparse hydrology progress

Branch: `water/0.1.0`

## Implemented

### Stable sparse identity and allocation

- `HydroTileKey`: cube face + quadtree level + Morton-addressable x/y identity.
- `HydroTilePool`: hard-bounded transient slot pool with recycling.
- `SparseHydroScheduler`: wake/settle/freeze/sleep hysteresis and physical-LOD ownership.
- Stable world identity remains separate from transient GPU slot identity.
- Frontier allocation now has a real two-stage lifecycle:

```text
reserve()
  -> ALLOCATING
  -> CPU owns slot
  -> GPU occupancy remains 0

initialize + conservative handoff

activate_reserved()
  -> ACTIVE
  -> tile_woken
  -> identity bridge publishes GPU metadata/occupancy
```

This prevents a new tile from becoming solver-visible before its state is valid.

### Exact cube-sphere topology

- `HydroTileTopology` derives same-level W/E/S/N neighbors from Asterra's actual
  equi-angular `CubeSphere` mapping.
- Cross-face links record destination edge and edge-index orientation.
- `HydroEdgeFrame` provides the CPU reference transform for seam-local momentum.

### GPU atlas and compact frontier

- `SparseHydroAtlasGPU`
  - FP32 A/B conservative state;
  - source terms;
  - occupancy;
  - stable `(face, level, x, y)` metadata per transient slot;
  - `stage_slot_state()` remains as a legacy/test CPU bootstrap path.
- `HydroTileActivityGPU`
  - compact per-tile depth, velocity, kinetic activity and wet/invalid count;
  - keeps **actual outward advective Q** on W/E/S/N;
  - additionally computes a one-sided dry-neighbor Rusanov **predictive wetting Q**.
- `HydroFrontierCandidatesGPU`
  - GPU-generated compact frontier queue;
  - uses `max(actual Q, predictive wetting Q)` for activation;
  - marks candidates whose predictive term dominated;
  - snapshots stable tile identity so delayed readback cannot act on a recycled slot.
- `HydroFrontierResolver`
  - maps compact queue entries back to stable topology;
  - rejects stale slot generations;
  - requires an explicit terrain/structure reachability callback;
  - can either wake immediately (legacy path) or reserve with deferred activation
    for conservative handoff.

The predictive wetting term fixes an important frontier edge case: stationary water
with hydrostatic head can request an adjacent dry tile even before a nonzero outward
`hu/hv` already exists. Reachability still filters cliffs, high banks, levees and
other impossible destinations.

### Resident connectivity

- `SparseHydroConnectivityGPU`
  - four resident-neighbor slots per source tile;
  - destination boundary encoding;
  - edge reversal bit;
  - nonresident neighbors remain `-1` and therefore temporarily reflective.

### Connected sparse SWE

- `shaders/water/sparse_hydro_step.glsl`
  - conservative h/hu/hv update over the sparse atlas;
  - hydrostatic reconstruction and Rusanov flux;
  - wet/dry positivity and Manning friction;
  - same-tile interior neighbors;
  - resident adjacent-tile boundary reads;
  - exact edge index reversal across cube seams;
  - destination `(hu,hv)` transformed into the source edge frame before solving
    the Riemann problem;
  - nonresident boundaries remain reflective until frontier allocation occurs.
- `shaders/water/sparse_hydro_commit.glsl`
  - keeps atlas A canonical after each sparse update during this integration stage.

### GPU adaptive CFL scheduler

- `shaders/water/sparse_hydro_reduce.glsl`
  - characteristic-speed and health reduction over **occupied slots only**;
  - ignores stale data in unoccupied/recycled storage.
- `shaders/water/sparse_hydro_prepare_step.glsl`
  - recomputes one safe CFL timestep before every candidate substep.
- existing `hydro_reset_reduction.glsl` is reused with the same 96-byte control ABI
  as the fixed-domain solver.
- `SparseHydroStepGPU.advance()` records:

```text
for each candidate substep:
    reset iteration reduction
      -> reduce occupied canonical atlas A
      -> compute fresh CFL-safe dt
      -> connected sparse SWE A -> B
      -> canonicalize B -> A

then:
    final occupied-state health reduction
      -> optional 96-byte asynchronous diagnostics
```

The scheduler never intentionally violates CFL. If `max_substeps` is insufficient,
it under-advances simulated time and reports `remaining_dt_s` + `cfl_clamped`.

### Conservative frontier handoff

Added:

```text
shaders/water/sparse_hydro_frontier_handoff.glsl
scripts/water/hydro_frontier_handoff_gpu.gd
scripts/water/hydro_frontier_activation_pipeline.gd
```

For a new same-level destination tile, the sequence is:

```text
GPU frontier candidate
      |
      v
exact topology + reachability
      |
      v
reserve destination slot (ALLOCATING, occupancy=0)
      |
      v
stage destination dry state + its OWN terrain bed into A and B
      |
      v
conservative GPU pre-wet handoff
      |
      v
activate destination
      |
      v
publish stable identity + occupancy
      |
      v
rebuild resident connectivity
      |
      v
next connected SWE advance
```

The handoff uses one invocation per mapped edge-cell pair. It:

1. hydrostatically reconstructs source depth against the destination bed;
2. predicts a small dry-neighbor Rusanov mass transfer for `seed_dt`;
3. clamps transfer to a configured fraction of source depth;
4. subtracts exactly `dh` from the source cell;
5. adds exactly `dh` to the destination mapped edge cell;
6. removes the same parcel fraction of existing source momentum;
7. rotates that parcel momentum into the destination cube-face frame and adds it.

For same-level cells this preserves water volume pairwise. Existing physical parcel
momentum is also preserved through the seam-frame transform. Pressure-generated
momentum is intentionally left to the normal Riemann/SWE update after connectivity
opens.

Multiple incoming frontier candidates for the same destination are grouped: the
destination is staged once, can receive conservative seeds from multiple source
edges, and is activated only after its final seed.

A failed/missing destination initializer cancels the reservation rather than
publishing arbitrary stale state.

### GPU terrain / bathymetry reconstruction

Added:

```text
shaders/water/hydro_terrain_bed_init.glsl
scripts/water/hydro_terrain_bed_gpu.gd
```

Production frontier allocation no longer needs to upload a complete dry bed/state
tile from CPU. `HydroTerrainBedGPU` samples the already-resident
`Planet.global_height_texture` directly on the main RenderingDevice and rebuilds
the same deterministic procedural detail spectrum as `gpu_terrain_height.gdshaderinc`.

For every cell in the reserved tile it computes cube-sphere cell-centre direction
from the stable `HydroTileKey`, evaluates pristine bed elevation, applies the sparse
runtime terrain-edit offset, and writes in one dispatch:

```text
atlas A = vec4(0, 0, 0, bed)
atlas B = vec4(0, 0, 0, bed)
sources = vec4(0)
```

The slot remains `ALLOCATING` and GPU occupancy remains zero throughout this pass.
The following conservative handoff is queued after the terrain dispatch; only after
seeding completes is identity/occupancy published.

The macro terrain and procedural bed are therefore GPU-only. Current `Deltas` are
still a sparse CPU dictionary, so the initializer uploads only a tiny
`tile_resolution^2` float offset patch for edited terrain. An unmodified tile sends
only zero offsets, not a complete height/state field. A future Deltas GPU mirror can
remove even this small transfer without changing the frontier lifecycle contract.

`HydroFrontierActivationPipeline` now accepts both:

- legacy `PackedFloat32Array` destination providers for isolated tests;
- queued GPU initializer dictionaries for the production terrain path.

## Tests added for Phase 3

CPU/headless policy/topology gates:

```text
tests/water/SparseHydroSchedulerTests.tscn
tests/water/HydroTileTopologyTests.tscn
tests/water/HydroFrontierResolverTests.tscn
```

Renderer/GPU gates:

```text
tests/water/SparseHydroGPUSmoke.tscn
tests/water/SparseHydroConnectedStep.tscn
tests/water/SparseHydroSeamStep.tscn
tests/water/SparseHydroAdaptiveSmoke.tscn
tests/water/HydroTerrainBedGPUSmoke.tscn
tests/water/SparseHydroFrontierHandoff.tscn
```

`SparseHydroGPUSmoke` verifies both actual boundary discharge and predictive
hydrostatic wetting candidates while ensuring unoccupied/dry slots produce none.

`SparseHydroConnectedStep` verifies water transfer across a resident same-face
boundary while combined water mass remains conserved.

`SparseHydroSeamStep` deliberately crosses a +X north edge into a polar cube face,
where local solver axes rotate. A physically uniform current is represented by two
different local momentum vectors; the seam must remain numerically quiet.

`SparseHydroAdaptiveSmoke` verifies:

- unoccupied stale high-speed storage does not affect CFL reduction;
- a 0.2 s requested macro step is split into several safe GPU substeps;
- the full requested time is advanced when the cap is sufficient;
- an intentionally oversized cap=1 request under-advances and reports CFL clamp;
- no invalid cells appear;
- closed-tile water mass remains conserved;
- canonical commit clears stale unoccupied state.

`HydroTerrainBedGPUSmoke` uses a synthetic six-face RF macro terrain with a distinct
constant elevation per face plus explicit sparse delta offsets. It verifies that:

- the correct cube face is sampled;
- h/hu/hv are reset to zero;
- bed equals macro elevation + delta;
- A and B receive the same initialized bed;
- source terms are cleared;
- no full CPU bed/state tile participates in reconstruction.

`SparseHydroFrontierHandoff` is the end-to-end self-expanding flood gate. It now uses
`HydroTerrainBedGPU` rather than a CPU-built flat destination state and validates:

```text
occupied source
  -> GPU activity + predictive wetting
  -> GPU frontier queue
  -> reachability permits one seam only
  -> destination reserved but unpublished
  -> GPU terrain bed reconstruction overwrites stale recycled storage
  -> conservative water transfer
  -> physical parcel momentum preserved across rotated face frame
  -> destination activated
  -> connectivity opened
  -> adaptive connected SWE continues transferring water
```

The test checks total mass before/after handoff and after the connected solver, and
checks the destination momentum mapped back into the source frame against the
momentum removed from the source.

## Runtime validation status

The current ChatGPT execution environment does not contain the project's Godot 4.7
renderer executable, so the renderer-mode scenes above are implemented but not
runtime-passed here.

Suggested local runs:

```text
godot --headless --path . tests/water/HydroTileTopologyTests.tscn
godot --headless --path . tests/water/SparseHydroSchedulerTests.tscn
godot --headless --path . tests/water/HydroFrontierResolverTests.tscn
godot --path . tests/water/SparseHydroGPUSmoke.tscn
godot --path . tests/water/SparseHydroConnectedStep.tscn
godot --path . tests/water/SparseHydroSeamStep.tscn
godot --path . tests/water/SparseHydroAdaptiveSmoke.tscn
godot --path . tests/water/HydroTerrainBedGPUSmoke.tscn
godot --path . tests/water/SparseHydroFrontierHandoff.tscn
```

Use the executable path/name appropriate for the local Godot 4.7 build.

## Next Phase 3 integration

The sparse flood can now allocate, reconstruct terrain, pre-wet, publish, and
continue solving without a full CPU bed upload. The next implementation should wrap
the individual pieces into one persistent runtime controller:

```text
carry pending simulation time
        |
        v
SparseHydroStepGPU.advance()
        |
        v
HydroTileActivityGPU.classify()
        |
        v
HydroFrontierCandidatesGPU.generate()
        |
        v
HydroFrontierActivationPipeline
  -> GPU terrain stage
  -> conservative handoff
  -> connectivity publish
        |
        v
feed activity into settle/sleep policy
        |
        +----> repeat with any CFL remainder
```

That controller should own macro simulation time, carry `remaining_dt_s` when a CFL
substep cap is hit, prevent overlapping GPU phases, and make frontier expansion an
ordinary part of every hydrology tick rather than test/manual orchestration.

After the runtime loop is stable, the next storage/terrain tasks are:

1. mirror sparse `Deltas` on GPU so edited offsets also require no CPU patch;
2. invalidate/reconstruct affected active hydrology tiles on `Deltas.region_changed`;
3. begin conservative physical-LOD restriction/prolongation/refluxing between
   different hydrology tile resolutions.
