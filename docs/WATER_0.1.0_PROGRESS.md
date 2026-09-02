# Water 0.1.0 implementation progress

Branch: `water/0.1.0`

## Current status

### Phase 0 — foundation: done

- `project.godot` reports 0.1.0.
- `WaterSystem` autoload added after the legacy `OceanSystem`.
- `PersistentHydrologySystem` is autoloaded after `WeatherSystem`.
- Full architecture plan lives in `docs/WATER_HYDROLOGY_0.1.0_IMPLEMENTATION_PLAN.md`.

### Phase 1 — rendering/physics ownership split: implemented, runtime validation pending

Implemented:

- `scripts/water/water_system.gd`
  - central water coordinator;
  - stable query/surface ownership boundary;
  - dynamic hydrology rendering remains OFF by default;
  - explicit debug/test controls can enable/recenter the dynamic coat.
- `scripts/water/hydrology_query_service.gd`
  - stable asynchronous gameplay query facade;
  - currently wraps `OceanGPUPhysics` without changing its behavior.
- `scripts/water/water_surface_resources.gd`
  - owns a 256x256 RGBA32F dynamic-water field on the **global** RenderingDevice;
  - exposes the same texture RID to spatial materials through `Texture2DRD`;
  - no GPU -> CPU -> GPU copy is required for production rendering.
- `shaders/water_dynamic_surface.gdshaderinc`
  - common contract for dynamic height, tangent velocity and activity/foam;
  - derives a metric surface gradient from the same height texture used for
    displacement so geometry and normals remain coherent.

### Phase 2 — fixed-domain shallow water: end-to-end GPU path implemented, runtime validation pending

#### Numerical core

- `scripts/water/hydro_reference_solver.gd`
  - CPU correctness oracle;
  - conservative `h`, `hu`, `hv` state;
  - hydrostatic reconstruction;
  - Rusanov numerical flux;
  - well-balanced bed-pressure correction;
  - positivity/wet-dry handling;
  - rainfall/infiltration;
  - Manning friction;
  - adaptive CFL substeps.
- `shaders/water/hydro_step.glsl`
  - GPU implementation of the same fixed-domain equations.

#### Adaptive GPU scheduler

- `shaders/water/hydro_reset_reduction.glsl`
- `shaders/water/hydro_reduce.glsl`
- `shaders/water/hydro_prepare_step.glsl`
- `shaders/water/hydro_finalize.glsl`
- `scripts/water/fixed_hydro_gpu.gd`

The macro advance remains GPU scheduled:

```text
for each candidate substep:
    reset characteristic-speed scratch
        -> reduce exact current state
        -> calculate one CFL-safe dt
        -> conditionally execute SWE step

then:
    canonicalize ping-pong parity
        -> final health reduction
        -> optional compact async diagnostics
```

CFL is recomputed after every active substep. If the configured substep cap is
insufficient, the solver under-advances simulated time and reports the remainder
instead of intentionally violating the stability limit.

#### Parity and mass diagnostics

- `scripts/water/hydro_state_readback.gd`
  - test-only asynchronous full-state readback.
- `shaders/water/hydro_mass_reduce.glsl`
- `shaders/water/hydro_mass_finalize.glsl`
- `scripts/water/hydro_volume_diagnostics_gpu.gd`
  - GPU-only water-volume reduction;
  - only a final four-byte FP32 volume is read back.

`tests/water/HydroGPUParityTests.tscn` compares GPU and CPU results across multiple
macro advances for lake-at-rest, wet/dry dam-break and uniform rain/infiltration.

#### SWE -> visible-water reconstruction

- `shaders/water/hydro_surface_reconstruct.glsl`
- `scripts/water/hydro_surface_reconstruction_gpu.gd`
- dynamic contribution in `shaders/ocean_geometry_clipmap.gdshader`

The shared RGBA32F reconstruction contract remains:

```text
R = (bed + h) - reference_surface
G = velocity X
B = velocity Y
A = activity / foam hint
```

`tests/water/HydroSurfaceReconstructionSmoke.tscn` verifies reconstruction through
the real shared texture. Production dynamic-water rendering remains disabled by
default and is bypassed exactly while `u_dynamic_surface_enabled = 0`.

#### Long closed-basin conservation soak

`tests/water/HydroGPUSoak.tscn` performs 10,000 macro advances by default and uses
only the four-byte GPU volume diagnostic at checkpoints. The initial release gate
is `relative drift <= 1e-4 OR absolute drift <= 0.08 m3`; tighten it from measured
results rather than broadening it to hide numerical errors.

### Phase 3 — sparse active hydrology: implemented through persistent ownership gate, runtime validation pending

#### Sparse GPU runtime

Already implemented:

- `SparseHydroScheduler` / `HydroTilePool` stable tile ownership;
- `SparseHydroAtlasGPU` A+B conservative state, source, occupancy and metadata;
- `SparseHydroConnectivityGPU`;
- `SparseHydroStepGPU`;
- GPU activity classification and frontier candidate extraction;
- exact cube-sphere frontier resolution;
- terrain staging directly on the global RenderingDevice;
- conservative fine->fine frontier handoff;
- point-source ingress;
- `SparseHydrologyRuntime` orchestration and CFL time-debt carry;
- `WaterSystem` production sparse bootstrap.

#### Persistent planetary store

Implemented:

- `scripts/water/planet_hydrology_store.gd`
  - slow conservative planet-wide soil/surface/channel state;
  - surface routing on the real `PlanetGrid` topology;
  - compact snapshots and promotion-candidate queries.
- `scripts/water/persistent_hydrology_system.gd`
  - production owner/autoload;
  - samples the CPU weather publication without GPU readback;
  - applies the same `WeatherSystem.simulation_weight` used by fine GPU forcing;
  - maintains a local committed weather-snapshot revision rather than pretending
    an asynchronous CPU snapshot is the newest native solver revision.

#### Transactional coarse <-> fine ownership

Implemented:

- `scripts/water/planet_hydrology_ownership_store.gd`
  - two-phase `prepare_promotion()` / `commit_promotion()`;
  - prepare reserves water but does not debit physical coarse storage;
  - rollback releases reservation with no physical state change;
  - over-reservation and duplicate commit fail closed;
  - coarse stepping pauses with `ERR_BUSY` while ownership is unresolved;
  - unresolved transactions are never serialized and block snapshots;
  - promotion/demotion have explicit representation-transfer ledger terms;
  - `accept_demotion()` returns conserved fine water to coarse storage.
- `scripts/water/hydro_coarse_seed_gpu.gd`
- `shaders/water/hydro_coarse_seed.glsl`
  - exact one-shot seed into an unpublished sparse tile;
  - writes identical depth/momentum parcel to state A and B;
  - leaves bed unchanged;
  - `plan_volume()` quantizes depth to the exact FP32 value sent to the GPU and
    reports the corresponding represented volume so CPU ownership debits the same
    parcel the GPU actually receives.
- `scripts/water/planet_hydro_promotion_bridge.gd`
  - sequences coarse reservation -> sparse reserve -> terrain stage -> exact seed
    -> coarse commit -> sparse activation/connectivity;
  - temporarily pauses the sparse runtime during the ownership handoff;
  - promotes only into a new unpublished tile for now;
  - restores coarse ownership and unpublishes/releases the fine tile if an
    activation/connectivity failure occurs after commit;
  - exposes a flood-oriented suggested parcel based on
    `coarse_surface_depth * one_fine_tile_area`, never the entire macro-cell volume.

Detailed contract:

```text
docs/WATER_PHASE3_PERSISTENT_OWNERSHIP.md
```

#### Phase 3 conservation gates

CPU-only:

```text
tests/water/PlanetHydrologyStoreTests.tscn
tests/water/test_planet_hydrology_store.gd
```

Checks closed conservation, exact precipitation accounting, snapshot round-trip,
reservation/rollback/commit behavior, snapshot fail-closed behavior, duplicate
commit rejection and promotion/demotion mass accounting using the real cube-sphere
`PlanetGrid`/`PlanetFields` constructors.

Renderer-mode exact-seed gate:

```text
tests/water/HydroCoarseSeedGPUSmoke.tscn
tests/water/test_hydro_coarse_seed_gpu.gd
```

Creates a one-slot sparse atlas, plans a known parcel, seeds both A and B, then
uses two independent GPU volume reducers to verify both ping-pong states contain
the planned represented volume.

## Validation status

The current ChatGPT execution environment does not contain the project Godot 4.7
binary, so Godot scenes have **not** been executed here. Static review and Godot
4.7 API matching have been performed; runtime success must not be inferred until
the scenes run on the project build/GPU.

Suggested local runs:

```text
godot --headless --path . tests/water/HydroReferenceTests.tscn
godot --headless --path . tests/water/Phase1WaterSmoke.tscn
godot --headless --path . tests/water/PlanetHydrologyStoreTests.tscn
godot --path . tests/water/FixedHydroGPUSmoke.tscn
godot --path . tests/water/HydroGPUParityTests.tscn
godot --path . tests/water/HydroSurfaceReconstructionSmoke.tscn
godot --path . tests/water/HydroGPUSoak.tscn
godot --path . tests/water/HydroCoarseSeedGPUSmoke.tscn
```

Shorter development soak example:

```text
godot --path . tests/water/HydroGPUSoak.tscn -- --hydro-soak-steps=1000
```

Use the executable name/path appropriate for the local Godot 4.7 build.

## Next gates

1. Run the CPU persistent-store gate and the renderer-mode exact-seed gate locally;
   fix actual parser/API/numerical failures without evidence-free tolerance changes.
2. Expose `PlanetHydroPromotionBridge` from `WaterSystem` only when both the sparse
   runtime and `PersistentHydrologySystem` store are ready.
3. Add an end-to-end promotion smoke scene that verifies:

```text
coarse_before
    == coarse_after_commit + GPU_seeded_fine_volume
```

and the inverse after demotion.
4. Add low-cadence automatic promotion for **surface/flood** candidates only.
   Channel-only promotion stays explicit until river/reach reconstruction exists.
5. Add representation-wide conservation diagnostics combining coarse storage,
   GPU-reduced active sparse water and exported outlet volume.
6. Replace uniform tile seeding with terrain-aware conservative prolongation while
   preserving the same exact-volume transaction/acknowledgement interface.
7. Separately run the existing Phase 2 GPU parity/reconstruction/soak gates and
   exercise the reconstructed coat in a normal playable coast scene before turning
   dynamic hydrology rendering on by default.
