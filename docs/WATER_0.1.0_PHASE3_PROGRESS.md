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
  - `stage_slot_state()` initializes both A and B while a slot remains unpublished,
    preventing stale recycled water from leaking into a new owner.
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
with hydrostatic head can now request an adjacent dry tile even before a nonzero
outward `hu/hv` already exists. Reachability still filters cliffs, high banks,
levees and other impossible destinations.

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

A failed/missing destination terrain initializer cancels the reservation rather
than publishing arbitrary stale state.

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
tests/water/SparseHydroFrontierHandoff.tscn
```

`SparseHydroGPUSmoke` now verifies both actual boundary discharge and predictive
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

`SparseHydroFrontierHandoff` is the first end-to-end self-expanding flood gate. It
uses a rotated +X -> polar-face seam and validates:

```text
occupied source
  -> GPU activity + predictive wetting
  -> GPU frontier queue
  -> reachability permits one seam only
  -> destination reserved but unpublished
  -> stale destination storage overwritten by staged dry terrain state
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
godot --path . tests/water/SparseHydroFrontierHandoff.tscn
```

Use the executable path/name appropriate for the local Godot 4.7 build.

## Next Phase 3 integration

The sparse flood can now expand conservatively, but the activation pipeline's
`destination_state_provider` is intentionally a temporary abstraction. The test
uses a CPU-built flat dry tile; production must not upload full terrain tiles from
CPU for every frontier wake.

The next implementation should therefore provide **GPU destination terrain/bathymetry
reconstruction** for an ALLOCATING slot:

```text
HydroTileKey(face, level, x, y)
          |
          v
planet/cube-sphere cell coordinates
          |
          v
procedural terrain + TerrainDeltas sampling
          |
          v
write dry vec4(0,0,0,bed) directly into atlas A+B
          |
          v
conservative frontier handoff
```

After that, wrap `advance -> classify -> frontier -> handoff -> connectivity` into a
persistent sparse-hydrology runtime loop and carry any CFL remainder forward rather
than requiring test/manual orchestration.
