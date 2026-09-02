# Water 0.1.0 implementation progress

Branch: `water/0.1.0`

## Current status

### Phase 0 — foundation: done

- `project.godot` reports 0.1.0.
- `WaterSystem` autoload added after the legacy `OceanSystem`.
- `PersistentHydrologySystem` is autoloaded after `WeatherSystem`.
- `HydroAutomaticSurfacePromotion` is autoloaded after coarse/fine weather ownership.
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

### Phase 3 — sparse active hydrology: ownership-safe promotion policy installed, runtime validation pending

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
    an asynchronous CPU snapshot is the newest native solver revision;
  - retains the latest **unmasked** native precipitation snapshot and reapplies
    spatial coarse/fine authority immediately when ownership fractions change, so
    the coarse store cannot run one interval with stale full-authority rainfall.

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

#### Frontier/rebuild failure hardening

The sparse frontier path now distinguishes reversible and irreversible ownership:

```text
no successful edge handoff
    -> coarse-only preseed is reversible
    -> restore coarse parcel
    -> release hidden destination

one or more successful edge handoffs
    -> source fine tile has already been debited
    -> destination owns irreversible transferred water
    -> publish/preserve destination
```

Additional hardening:

- coarse-store rebuild is a hard sparse-generation boundary; the old atlas and
  helpers are destroyed before anything binds to the replacement store;
- a failed sparse runtime is torn down as one generation so an asynchronous hidden
  destination cannot activate after its coarse parcel was restored;
- multi-destination frontier failure cleanup is intrinsic to the activation
  pipeline rather than depending on a later `WaterSystem` teardown;
- failed preseed rollback can defer and retry a coarse restoration blocked by an
  unrelated ownership transaction;
- provisional coarse state is finalized only after the destination is actually
  published by the scheduler.

`WaterSystem.gpu_stats()` exposes pending/provisional/deferred preseed counts for
these failure paths.

#### Automatic surface/flood promotion policy — implemented, production switch OFF

- `scripts/water/hydro_automatic_surface_promotion.gd`
- autoload: `HydroAutomaticSurfacePromotion`
- controlling switch remains:

```gdscript
WaterSystem.automatic_coarse_promotion_enabled = false
```

Policy behavior:

- scans at low cadence (3 s default);
- uses only coarse **surface-storage depth** for eligibility; discharge is given an
  effectively unreachable threshold so channel-only anomalies cannot enter;
- defaults to 5 cm enter / 2.5 cm exit hysteresis;
- starts at most one promotion transaction per scan;
- refuses to start while any coarse ownership transaction is unresolved;
- maps each coarse cell to its exact sparse tile and suppresses any tile already
  present in `HydroTilePool`, including allocating/active/settling/frozen states;
- caps the requested parcel to current coarse surface storage, so the automatic
  path cannot borrow channel storage;
- seeds zero initial tangent velocity until the coarse model owns a trustworthy 2D
  local flood-velocity field;
- clears policy-local latches/identity on coarse-store rebuild;
- exposes counters/reasons through `HydroAutomaticSurfacePromotion.stats()`;
- exposes deterministic `scan_once()` plus a pre-tree dependency-injection seam so
  the renderer gate exercises the same production policy logic rather than a copy.

Channel-only automatic promotion remains explicitly disabled until the river/reach
representation exists.

#### Phase 3 conservation and policy gates

CPU/headless:

```text
tests/water/PlanetHydrologyStoreTests.tscn
tests/water/HydroFrontierFailurePolicyTests.tscn
tests/water/HydroPrecipitationOwnershipTests.tscn
tests/water/HydroAutomaticSurfacePromotionTests.tscn
```

The automatic-promotion policy gate checks threshold hysteresis and proves the
requested automatic parcel is bounded by surface storage rather than channel water.
The frontier failure-policy gate locks the reversible/irreversible classification.

Renderer-mode:

```text
tests/water/HydroCoarseSeedGPUSmoke.tscn
tests/water/PlanetHydroPromotionBridgeTests.tscn
tests/water/HydroAutomaticSurfacePromotionGPU.tscn
tests/water/HydroRepresentationAuditTests.tscn
tests/water/SparseHydroFrontierHandoff.tscn
```

`HydroAutomaticSurfacePromotionGPU.tscn` constructs a known flooded coarse cell,
runs the real policy + real transactional promotion bridge, reduces authoritative
occupied sparse volume on GPU, and checks:

```text
coarse_before
    == coarse_after_commit + GPU_seeded_fine_volume
```

It also verifies channel storage is unchanged and a second scan cannot duplicate the
promotion because the mapped fine tile is already resident.

## Validation status

The current ChatGPT execution environment does not contain the project Godot 4.7
binary, so Godot scenes have **not** been executed here. Static review and API/path
matching have been performed; runtime success must not be inferred until the scenes
run on the project build/GPU.

Suggested local runs:

```text
godot --headless --path . tests/water/HydroReferenceTests.tscn
godot --headless --path . tests/water/Phase1WaterSmoke.tscn
godot --headless --path . tests/water/PlanetHydrologyStoreTests.tscn
godot --headless --path . tests/water/HydroFrontierFailurePolicyTests.tscn
godot --headless --path . tests/water/HydroPrecipitationOwnershipTests.tscn
godot --headless --path . tests/water/HydroAutomaticSurfacePromotionTests.tscn
godot --path . tests/water/FixedHydroGPUSmoke.tscn
godot --path . tests/water/HydroGPUParityTests.tscn
godot --path . tests/water/HydroSurfaceReconstructionSmoke.tscn
godot --path . tests/water/HydroGPUSoak.tscn
godot --path . tests/water/HydroCoarseSeedGPUSmoke.tscn
godot --path . tests/water/PlanetHydroPromotionBridgeTests.tscn
godot --path . tests/water/HydroAutomaticSurfacePromotionGPU.tscn
godot --path . tests/water/HydroRepresentationAuditTests.tscn
godot --path . tests/water/SparseHydroFrontierHandoff.tscn
```

Shorter development soak example:

```text
godot --path . tests/water/HydroGPUSoak.tscn -- --hydro-soak-steps=1000
```

Use the executable name/path appropriate for the local Godot 4.7 build.

## Next gates

1. Run the new headless ownership/failure/promotion-policy gates and the renderer
   automatic-promotion conservation gate locally; fix actual parser/API/numerical
   failures without evidence-free tolerance changes.
2. Extend the automatic-promotion smoke through a genuinely conservative
   fine->coarse collapse/demotion path before installing automatic deactivation.
3. Complete the representation-wide conservation ledger for fine external source
   terms (rain/gameplay) so coarse + fine + exported outlet volume can be audited
   during normal production simulation, not only controlled no-source gates.
4. Replace uniform promotion seeding with terrain-aware conservative prolongation
   while preserving the exact-volume transaction/acknowledgement interface.
5. Build the persistent 1D river/reach representation; only then allow channel-only
   anomalies to promote automatically into sparse 2D SWE.
6. Separately run the Phase 2 GPU parity/reconstruction/soak gates and exercise the
   reconstructed coat in a normal playable coast scene before dynamic hydrology
   rendering is enabled by default.
