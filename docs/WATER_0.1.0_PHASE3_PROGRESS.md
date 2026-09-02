# Water 0.1.0 — Phase 3 sparse hydrology progress

Branch: `water/0.1.0`

## Implemented

### Stable sparse identity and allocation

- `HydroTileKey`: cube face + quadtree level + Morton-addressable x/y identity.
- `HydroTilePool`: hard-bounded transient slot pool with recycling.
- `SparseHydroScheduler`: wake/settle/freeze/sleep hysteresis and physical-LOD ownership.
- Stable world identity remains separate from transient GPU slot identity.

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
  - stable `(face, level, x, y)` metadata per transient slot.
- `HydroTileActivityGPU`
  - compact per-tile depth, velocity, kinetic activity, wet/invalid count and
    directional outgoing flux reduction.
- `HydroFrontierCandidatesGPU`
  - GPU-generated compact boundary-flux queue;
  - queue entries snapshot stable tile identity so delayed readback cannot act on
    a recycled slot.
- `HydroFrontierResolver`
  - maps compact queue entries back to stable topology;
  - rejects stale slot generations;
  - requires an explicit terrain/structure reachability callback before waking a
    destination tile.

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
- `SparseHydroStepGPU.advance()` now records:

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

The normal gameplay path still performs no full-state readback.

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
```

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
```

Use the executable path/name appropriate for the local Godot 4.7 build.

## Next Phase 3 integration

The numerical sparse solver and adaptive scheduler are now separate from frontier
activation only by orchestration. The next runtime layer should chain:

```text
SparseHydroStepGPU.advance(macro_dt)
          |
          v
HydroTileActivityGPU.classify()
          |
          v
HydroFrontierCandidatesGPU.generate()
          |
          v
HydroFrontierResolver
          |
          +--> blocked boundary: no allocation
          |
          +--> reachable boundary: wake destination tile
                                   |
                                   v
                         initialize new tile state
                                   |
                                   v
                      rebuild resident connectivity
                                   |
                                   v
                         next sparse macro advance
```

The missing critical piece before calling this a self-expanding flood is **new tile
initialization**. A newly woken destination cannot start as an arbitrary zero-depth
reflective tile after source flux has already reached the boundary. The next step
must define a conservative frontier handoff/initialization rule and then connect the
orchestration loop around it.
