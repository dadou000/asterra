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

The dynamic RenderingDevice field is intentionally unavailable in Godot headless
mode; the test treats that as the documented fallback rather than a failure.

### Phase 2 — fixed-domain shallow water: in progress

Implemented:

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
  - ping-pong state buffers;
  - closed reflective test boundaries.
- `scripts/water/fixed_hydro_gpu.gd`
  - global-RenderingDevice compute dispatcher;
  - render-thread resource creation, dispatch and destruction;
  - two symmetric SSBO uniform sets for A -> B and B -> A;
  - no `submit()` or `sync()` calls on the global device;
  - exposes the current conservative-state RID for later reconstruction kernels.

Numerical reference scene:

```text
tests/water/HydroReferenceTests.tscn
```

It checks:

1. lake-at-rest over uneven bathymetry;
2. dam-break positivity and mass conservation;
3. exact uniform-rain volume accounting.

GPU pipeline smoke scene:

```text
tests/water/FixedHydroGPUSmoke.tscn
```

It checks shader/pipeline creation, storage-buffer bindings, one compute dispatch
and ping-pong ownership. It requires a real RenderingDevice renderer and skips in
headless/Compatibility mode.

## Validation status

The current ChatGPT execution environment does not contain a Godot executable, so
these Godot scenes have **not** been executed here. Static review and API matching
have been performed, but runtime success must not be inferred until the scenes are
run with the project's Godot 4.7 build.

Suggested local commands:

```text
godot --headless --path . tests/water/HydroReferenceTests.tscn
godot --headless --path . tests/water/Phase1WaterSmoke.tscn
godot --path . tests/water/FixedHydroGPUSmoke.tscn
```

Use the executable name/path appropriate for the local Godot 4.7 build.

## Next Phase 2 tasks

1. Add GPU reduction for max characteristic speed, wet-cell count, invalid-cell
   count and max depth.
2. Derive CFL timestep from that reduction and support multiple ordered GPU
   substeps without CPU synchronization.
3. Add a compact asynchronous debug readback path for tests only.
4. Compare GPU output numerically against `HydroReferenceSolver` for the three
   reference fixtures.
5. Add fixed-domain dynamic-height reconstruction into the shared water texture.
6. Only after parity/stability: begin sparse tile allocation and active-frontier
   scheduling (Phase 3).
