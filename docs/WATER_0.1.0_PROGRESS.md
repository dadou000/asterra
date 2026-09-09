# Water 0.1.0 implementation progress

Branch: `water/0.1.0`

## Current status

### Phase 0 — foundation: done

- `project.godot` reports 0.1.0.
- `WaterSystem` autoload added after the legacy `OceanSystem`.
- `PersistentHydrologySystem` is autoloaded after `WeatherSystem`.
- `HydroAutomaticSurfacePromotion` and `HydroAutomaticSurfaceDemotion` are
  autoloaded after coarse/fine weather ownership.
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

### Phase 3 — sparse active hydrology: bidirectional surface ownership installed, runtime validation pending

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
    spatial coarse/fine authority immediately when ownership fractions change.

#### Transactional coarse -> fine ownership

- `scripts/water/planet_hydrology_ownership_store.gd`
  - two-phase `prepare_promotion()` / `commit_promotion()`;
  - prepare reserves water but does not debit physical coarse storage;
  - rollback releases reservation with no physical state change;
  - over-reservation and duplicate commit fail closed;
  - unresolved ownership blocks coarse stepping and snapshots.
- `scripts/water/hydro_coarse_seed_gpu.gd`
- `shaders/water/hydro_coarse_seed.glsl`
  - exact one-shot seed into an unpublished sparse tile;
  - writes identical depth/momentum parcel to state A and B;
  - `plan_volume()` quantizes to the exact FP32 depth represented on the GPU.
- `scripts/water/planet_hydro_promotion_bridge.gd`
  - coarse reserve -> sparse reserve -> terrain stage -> exact seed -> coarse commit
    -> sparse publication/connectivity;
  - promotes only into a new unpublished tile;
  - flood-oriented default parcel is `coarse_surface_depth * one_fine_tile_area`.

#### Transactional fine -> coarse ownership

Implemented:

- `PlanetHydrologyOwnershipStore.prepare_demotion()` / `commit_demotion()` /
  `rollback_demotion()`;
  - incoming fine parcels are validated without changing coarse physical storage;
  - pending demotion blocks coarse stepping and snapshots;
  - promotion and demotion transactions are mutually exclusive;
  - `accept_demotion()` remains a synchronous compatibility wrapper around the
    two-phase incoming transaction.
- `shaders/water/sparse_hydro_tile_mass_reduce.glsl`
- `scripts/water/sparse_hydro_tile_volume_diagnostics_gpu.gd`
  - reduces one occupied canonical atlas-A tile to one four-byte FP32 volume;
  - no full h/hu/hv/bed readback is required.
- `scripts/water/planet_hydro_demotion_bridge.gd`
  - pause sparse runtime -> reduce exact fine tile -> prepare incoming coarse parcel
    -> unpublish fine tile -> connectivity sync -> coarse commit;
  - positive measured volume is transactional by default; the dry threshold is zero,
    so tiny positive parcels are not intentionally discarded;
  - if fine publication must be restored after a late failure, the just-freed slot
    is reacquired while its raw GPU state is still intact;
  - if fine restoration itself fails after unpublication, the already-validated
    coarse incoming transaction is committed as the conservative fallback.
- `WaterSystem.demote_fine_surface_cell(cell)` exposes the explicit reverse path and
  production promotion/demotion calls are single-flight against each other.

Detailed ownership contract:

```text
docs/WATER_PHASE3_PERSISTENT_OWNERSHIP.md
```

#### Frontier/rebuild/runtime-failure hardening

The frontier path distinguishes reversible and irreversible ownership:

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

- precipitation authority changes reapply immediately to the retained unmasked
  native weather snapshot;
- failed preseed rollback can defer/retry a coarse restoration blocked by another
  ownership transaction;
- provisional coarse frontier state is finalized only after destination publication;
- multi-destination frontier failure cleanup is intrinsic to the activation pipeline;
- **failed sparse generations that still have allocated fine tiles are preserved in
  place rather than automatically destroying their atlas**. This prevents a runtime
  failure from becoming a representation-mass deletion;
- an in-flight fine->coarse demotion is allowed to finish before failed-generation
  teardown can proceed;
- an empty failed generation may still be torn down automatically.

`WaterSystem.gpu_stats()` reports the promotion/demotion bridges, frontier preseed
state, compact diagnostics, and `failed_generation_preserved`.

#### Automatic surface/flood promotion — installed, production switch OFF

- `scripts/water/hydro_automatic_surface_promotion.gd`
- autoload: `HydroAutomaticSurfacePromotion`
- switch:

```gdscript
WaterSystem.automatic_coarse_promotion_enabled = false
```

Policy:

- 3 s scan cadence;
- 5 cm enter / 2.5 cm exit surface-depth hysteresis;
- channel-only anomalies cannot enter;
- at most one transaction per scan;
- already resident/allocating/settling/frozen mapped tiles are suppressed;
- automatic parcel is capped by coarse **surface** storage and cannot borrow channel
  storage;
- zero initial tangent velocity until coarse hydrology has a trustworthy 2D flow
  direction/velocity representation;
- successful policy-owned tiles are explicitly registered with the automatic
  demotion policy; manual/frontier/source tiles are never registered through this
  path.

#### Automatic quiet surface demotion — installed, production switch OFF

- `scripts/water/hydro_automatic_surface_demotion.gd`
- autoload: `HydroAutomaticSurfaceDemotion`
- switch:

```gdscript
WaterSystem.automatic_fine_demotion_enabled = false
```

Initial policy is deliberately restrictive:

- only tiles created by `HydroAutomaticSurfacePromotion` are eligible;
- 5 s scan cadence;
- only `SETTLING` or `FROZEN_WATER` records;
- default minimum quiet time: 20 s;
- default maximum fine depth: 0.15 m;
- default maximum velocity: 0.004 m/s;
- default maximum outgoing flux: 0.002 m3/s;
- default maximum disturbance energy: 2.5e-5;
- any resident cardinal neighbor suppresses collapse, including a neighbor across a
  cube-face seam;
- at most one fine->coarse transaction per scan;
- failed attempts receive a scan-count retry delay;
- world/sparse-generation identity changes clear the policy registry.

This intentionally does **not** collapse connected fine flood networks, river/source
regions, manual fine domains or arbitrary scheduler tiles. Cluster-aware collapse is
a later representation policy.

#### Phase 3 conservation and policy gates

CPU/headless:

```text
tests/water/PlanetHydrologyStoreTests.tscn
tests/water/PlanetHydroDemotionTransactions.tscn
tests/water/HydroFrontierFailurePolicyTests.tscn
tests/water/HydroPrecipitationOwnershipTests.tscn
tests/water/HydroAutomaticSurfacePromotionTests.tscn
tests/water/HydroAutomaticSurfaceDemotionTests.tscn
```

The demotion transaction gate checks prepare/rollback/commit, step/save blocking,
mutual exclusion with promotion and duplicate-commit rejection. The automatic
collapse policy gate locks quiet/activity limits and checks cardinal-neighbor
suppression across a cube-face seam.

Renderer-mode:

```text
tests/water/HydroCoarseSeedGPUSmoke.tscn
tests/water/PlanetHydroPromotionBridgeTests.tscn
tests/water/HydroAutomaticSurfacePromotionGPU.tscn
tests/water/PlanetHydroDemotionBridgeTests.tscn
tests/water/HydroAutomaticSurfaceDemotionGPU.tscn
tests/water/HydroRepresentationAuditTests.tscn
tests/water/SparseHydroFrontierHandoff.tscn
```

`PlanetHydroDemotionBridgeTests.tscn` performs a complete exact-volume
coarse->fine->coarse round trip and checks final occupied sparse volume is zero.

`HydroAutomaticSurfaceDemotionGPU.tscn` performs a real coarse->fine promotion,
marks the policy-owned scheduler record as a quiet shallow settling tile, runs the
actual automatic-collapse policy, and verifies:

```text
coarse_final == coarse_initial
fine_authoritative_final == 0
cumulative_promoted_to_fine == cumulative_demoted_from_fine
mass_error == 0
```

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
godot --headless --path . tests/water/PlanetHydroDemotionTransactions.tscn
godot --headless --path . tests/water/HydroFrontierFailurePolicyTests.tscn
godot --headless --path . tests/water/HydroPrecipitationOwnershipTests.tscn
godot --headless --path . tests/water/HydroAutomaticSurfacePromotionTests.tscn
godot --headless --path . tests/water/HydroAutomaticSurfaceDemotionTests.tscn
godot --path . tests/water/FixedHydroGPUSmoke.tscn
godot --path . tests/water/HydroGPUParityTests.tscn
godot --path . tests/water/HydroSurfaceReconstructionSmoke.tscn
godot --path . tests/water/HydroGPUSoak.tscn
godot --path . tests/water/HydroCoarseSeedGPUSmoke.tscn
godot --path . tests/water/PlanetHydroPromotionBridgeTests.tscn
godot --path . tests/water/HydroAutomaticSurfacePromotionGPU.tscn
godot --path . tests/water/PlanetHydroDemotionBridgeTests.tscn
godot --path . tests/water/HydroAutomaticSurfaceDemotionGPU.tscn
godot --path . tests/water/HydroRepresentationAuditTests.tscn
godot --path . tests/water/SparseHydroFrontierHandoff.tscn
```

Shorter development soak example:

```text
godot --path . tests/water/HydroGPUSoak.tscn -- --hydro-soak-steps=1000
```

Use the executable name/path appropriate for the local Godot 4.7 build.

## Next gates

1. Run the new bidirectional ownership and automatic-policy gates locally; fix real
   parser/API/numerical failures without broadening tolerances speculatively.
2. Add cumulative ledgers for fine external source terms (atmospheric precipitation
   and gameplay/world sources) so representation-wide audits close during normal
   production forcing rather than only controlled no-source tests.
3. Add cluster-aware fine->coarse collapse for connected quiet surface domains;
   isolated-tile automatic demotion must remain the only automatic reverse policy
   until that conservative multi-tile transaction exists.
4. Replace uniform promotion seeding with terrain-aware conservative prolongation
   while preserving the exact-volume transaction/acknowledgement interface.
5. Build the persistent 1D river/reach representation; only then allow channel-only
   anomalies to promote/collapse automatically between 1D and sparse 2D SWE.
6. Add explicit save/world-transition ownership flushing instead of relying on world
   rebuild teardown when fine gameplay state must persist across a transition.
7. Separately run the Phase 2 GPU parity/reconstruction/soak gates and exercise the
   reconstructed coat in a normal playable coast scene before dynamic hydrology
   rendering is enabled by default.
