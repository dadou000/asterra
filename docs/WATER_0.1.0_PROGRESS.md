# Water 0.1.0 implementation progress

Branch: `water/0.1.0`

## Current status

### Phase 0 — foundation: done

- `project.godot` reports 0.1.0.
- `WaterSystem` autoload added after the legacy `OceanSystem`.
- Full architecture plan lives in `docs/WATER_HYDROLOGY_0.1.0_IMPLEMENTATION_PLAN.md`.

### Phase 1 — rendering/physics ownership split: implemented, runtime validation pending

Implemented:

- `scripts/water/water_system.gd`
  - central 0.1.0 water coordinator;
  - preserves the existing ocean path during migration.
- `scripts/water/hydrology_query_service.gd`
  - stable asynchronous gameplay query facade;
  - currently wraps `OceanGPUPhysics` without changing its behavior.
- `scripts/water/water_surface_resources.gd`
  - owns a 256x256 RGBA32F dynamic-water field on the **global** RenderingDevice;
  - all global-RD mutations are queued with `RenderingServer.call_on_render_thread()`;
  - exposes the RID through `Texture2DRD`, avoiding GPU -> CPU -> GPU copies.
- `shaders/water_dynamic_surface.gdshaderinc`
  - common shader contract for dynamic height, tangent velocity and activity/foam.
- `shaders/water_surface.gdshaderinc`
  - imports the dynamic-water contract.
- `WaterSystem` binds the dynamic texture to the existing ocean material but keeps
  `u_dynamic_surface_enabled = 0.0`, so this phase cannot alter current visuals.

Smoke scene:

```text
tests/water/Phase1WaterSmoke.tscn
```

### Phase 2 — fixed-domain shallow water: adaptive GPU scheduler implemented, parity/reconstruction pending

Implemented numerical core:

- `scripts/water/hydro_reference_solver.gd`
  - CPU correctness oracle;
  - conservative `h`, `hu`, `hv` state;
  - hydrostatic reconstruction;
  - Rusanov numerical flux;
  - well-balanced bed-pressure source correction;
  - positivity/wet-dry handling;
  - rainfall and infiltration source terms;
  - semi-implicit Manning friction;
  - CFL substepping.
- `shaders/water/hydro_step.glsl`
  - compute implementation of the same fixed-domain equations;
  - one invocation per cell;
  - closed reflective test boundaries;
  - consumes the current GPU-selected timestep;
  - candidate dispatches become no-ops after requested macro time is consumed.

Implemented adaptive GPU scheduling/diagnostics:

- `shaders/water/hydro_reset_reduction.glsl`
  - resets only per-iteration characteristic-speed scratch.
- `shaders/water/hydro_reduce.glsl`
  - 8x8 shared-memory workgroup reduction;
  - current maximum characteristic speed/depth;
  - wet-cell and invalid-cell counts;
  - separate final post-step health metrics.
- `shaders/water/hydro_prepare_step.glsl`
  - runs before **every** candidate SWE substep;
  - recomputes CFL-safe `dt` from the state produced by the preceding step;
  - tracks requested, advanced and remaining macro time;
  - tracks minimum/last CFL timestep and actual executed step count;
  - if the configured cap is exhausted, leaves the remainder unadvanced and sets
    `cfl_clamped=1` instead of violating stability.
- `shaders/water/hydro_finalize.glsl`
  - canonicalizes odd ping-pong results back into the original authoritative RID;
  - consumers do not need a CPU readback to discover which state buffer is final.
- `scripts/water/fixed_hydro_gpu.gd`
  - global-RenderingDevice compute dispatcher;
  - render-thread resource creation, dispatch and destruction;
  - uses a RID bundle for initialization/failure cleanup;
  - records one macro advance as:

```text
for each candidate substep:
    reset iteration reduction
        -> reduce current state
        -> prepare one CFL-safe dt
        -> conditionally execute SWE step

then:
    canonicalize ping-pong parity
        -> reduce final state
        -> 96-byte async diagnostic readback
```

  - CFL is therefore not stale across accelerating flood/dam-break flow;
  - all dependent dispatches are separated by compute barriers;
  - no `submit()` or `sync()` calls on the global RenderingDevice;
  - `advance(dt, max_substeps, request_diagnostics)` is the main scheduler API;
  - `step(dt)` remains a compatibility wrapper with a one-substep cap;
  - exposes current conservative-state and control-buffer RIDs for later kernels.

Numerical reference scene:

```text
tests/water/HydroReferenceTests.tscn
```

It checks:

1. lake-at-rest over uneven bathymetry;
2. dam-break positivity and mass conservation;
3. exact uniform-rain volume accounting.

GPU pipeline/scheduler scene:

```text
tests/water/FixedHydroGPUSmoke.tscn
```

It now checks:

1. all fixed-domain compute pipelines initialize;
2. GPU reduction reports the expected wet/invalid cell counts;
3. a 1 s macro timestep is split into multiple CFL-safe GPU substeps;
4. the canonical state RID remains stable after unknown odd/even GPU step count;
5. an intentionally huge timestep with a two-substep cap reports `cfl_clamped`;
6. the clamped case advances less than the requested timestep instead of violating
   the stability bound;
7. post-step invalid-cell count remains zero;
8. compact asynchronous diagnostic readback completes.

## Validation status

The current ChatGPT execution environment does not contain a Godot executable, so
these Godot scenes have **not** been executed here. Static review and Godot 4.7 API
matching have been performed, but runtime success must not be inferred until the
scenes are run with the project's Godot 4.7 build.

Suggested local commands:

```text
godot --headless --path . tests/water/HydroReferenceTests.tscn
godot --headless --path . tests/water/Phase1WaterSmoke.tscn
godot --path . tests/water/FixedHydroGPUSmoke.tscn
```

Use the executable name/path appropriate for the local Godot 4.7 build.

## Next Phase 2 tasks

1. Add a test-only async state-buffer readback path.
2. Compare GPU state numerically against `HydroReferenceSolver` for lake-at-rest,
   dam break and rainfall fixtures over multiple steps.
3. Add mass/volume reduction to GPU diagnostics so long GPU-only soak tests can
   track unexplained volume drift without reading the state grid.
4. Add fixed-domain state -> dynamic-height/velocity reconstruction into the
   shared `WaterSurfaceResources` texture.
5. Enable the reconstructed contribution in a dedicated test scene, not in the
   production ocean by default.
6. Only after parity/stability: begin sparse tile allocation and active-frontier
   scheduling (Phase 3).
