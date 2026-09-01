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

### Phase 2 — fixed-domain shallow water: numerical parity gate implemented, reconstruction pending

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

Implemented adaptive GPU scheduling/health diagnostics:

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
  - CFL is recomputed on the exact state produced by each prior active substep;
  - all dependent dispatches are separated by compute barriers;
  - no `submit()` or `sync()` calls on the global RenderingDevice;
  - `advance(dt, max_substeps, request_diagnostics)` is the main scheduler API.

Implemented parity/debug instrumentation:

- `scripts/water/hydro_state_readback.gd`
  - explicit debug/test-only full conservative-state readback;
  - asynchronous `buffer_get_data_async()` path;
  - one request at a time;
  - production gameplay does not use this path.
- `shaders/water/hydro_mass_reduce.glsl`
  - pairwise 8x8 workgroup depth reduction;
  - writes one FP32 partial sum per workgroup;
  - no floating-point atomics required.
- `shaders/water/hydro_mass_finalize.glsl`
  - reduces the small partial buffer on GPU;
  - converts depth sum to cubic metres using cell area.
- `scripts/water/hydro_volume_diagnostics_gpu.gd`
  - separate GPU-only volume diagnostic pipeline;
  - only the final four-byte volume value is read to CPU;
  - solver CFL/control buffers are untouched by instrumentation.

Reference-only numerical scene:

```text
tests/water/HydroReferenceTests.tscn
```

GPU pipeline/scheduler smoke scene:

```text
tests/water/FixedHydroGPUSmoke.tscn
```

GPU/CPU parity scene:

```text
tests/water/HydroGPUParityTests.tscn
```

The parity scene runs multiple macro advances for:

1. lake-at-rest over uneven bathymetry;
2. wet/dry dam break;
3. uniform rainfall + infiltration.

For each fixture it verifies:

- cell-by-cell `h`, `hu`, `hv` against `HydroReferenceSolver`;
- bed elevation remains unchanged;
- GPU-only reduced water volume agrees with the CPU reference;
- GPU-only reduced volume agrees with volume reconstructed from the full debug
  state readback, independently checking reduction indexing/layout.

## Validation status

The current ChatGPT execution environment does not contain a Godot executable, so
these Godot scenes have **not** been executed here. Static review and Godot 4.7 API
matching have been performed, but runtime success must not be inferred until the
scenes are run with the project's Godot 4.7 build.

Official Godot 4.7 documentation confirms the RenderingDevice API used here:
`buffer_get_data_async()` and `compute_list_add_barrier()` are available on the
modern RenderingDevice path. RenderingDevice remains unavailable in headless and
Compatibility rendering modes.

Suggested local commands:

```text
godot --headless --path . tests/water/HydroReferenceTests.tscn
godot --headless --path . tests/water/Phase1WaterSmoke.tscn
godot --path . tests/water/FixedHydroGPUSmoke.tscn
godot --path . tests/water/HydroGPUParityTests.tscn
```

Use the executable name/path appropriate for the local Godot 4.7 build.

## Next Phase 2 tasks

1. Run the parity scene on the project GPU and tune only evidence-based tolerance
   differences; numerical mismatches must be fixed, not hidden by broad tolerances.
2. Add fixed-domain state -> dynamic-height/velocity reconstruction into the
   shared `WaterSurfaceResources` texture.
3. Enable the reconstructed contribution in a dedicated visual test scene only;
   the production ocean remains at `u_dynamic_surface_enabled = 0.0` until the
   reconstruction and parity tests pass.
4. Add a long GPU-only closed-basin soak test using the four-byte volume diagnostic
   to measure accumulated mass drift without full-grid readbacks.
5. Only after parity/stability: begin sparse tile allocation and active-frontier
   scheduling (Phase 3).
