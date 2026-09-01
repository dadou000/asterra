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
macro advances for:

1. lake at rest over uneven bathymetry;
2. wet/dry dam break;
3. uniform rain + infiltration.

It verifies cell-by-cell `h/hu/hv`, unchanged bed elevation and two independent
GPU-volume checks.

#### SWE -> visible-water reconstruction

Implemented:

- `shaders/water/hydro_surface_reconstruct.glsl`
  - reads `vec4(h, hu, hv, bed)` directly from the conservative GPU state buffer;
  - maps the fixed-domain metric grid into the shared local tangent-plane cache;
  - writes:

```text
R = (bed + h) - reference_surface
G = velocity X
B = velocity Y
A = activity / foam hint
```

  - dry/out-of-domain texels become zero;
  - source state is bilinearly reconstructed into the render cache.
- `scripts/water/hydro_surface_reconstruction_gpu.gd`
  - global-RenderingDevice compute dispatcher;
  - binds the solver SSBO and `WaterSurfaceResources` texture as a storage image;
  - source and target resources remain externally owned;
  - reconstruction requires no CPU readback.
- `shaders/ocean_geometry_clipmap.gdshader`
  - the toroidal ocean coat now samples the dynamic hydrology texture;
  - dynamic height contributes to actual vertex displacement;
  - reconstructed height changes local water depth used by wave shoaling;
  - finite-difference dynamic gradient contributes to the same resolved surface
    normal as the FFT/interaction wave field;
  - hydraulic velocity/activity can contribute to local foam response;
  - all of the above is exactly bypassed while `u_dynamic_surface_enabled = 0`.
- `scripts/water/water_system.gd`
  - production default remains disabled;
  - `set_dynamic_surface_render_enabled()` is the explicit opt-in gate;
  - `set_dynamic_surface_center_plane()` keeps compute/render metric frames aligned.

Reconstruction smoke scene:

```text
tests/water/HydroSurfaceReconstructionSmoke.tscn
```

It creates a known Gaussian free-surface disturbance with nonzero momentum,
reconstructs it directly into the shared RGBA32F texture, enables the real ocean
material for the test, and then performs a test-only texture readback verifying:

- reconstructed maximum surface residual;
- velocity channels;
- activity channel;
- source-domain masking;
- finite values;
- the ocean material actually received the dynamic-surface enable.

The test disables the dynamic coat again before exiting.

## Validation status

The current ChatGPT execution environment does not contain the project Godot 4.7
binary, so renderer-mode tests have **not** been executed here. Static review and
Godot RenderingDevice API matching have been performed; runtime success must not
be inferred until the scenes run on the project renderer/GPU.

The Godot 4.7 RenderingDevice path used here supports storage images via
`UNIFORM_TYPE_IMAGE` and textures carrying `TEXTURE_USAGE_STORAGE_BIT`, as well as
the asynchronous buffer/texture readbacks used only by tests.

Suggested local runs:

```text
godot --headless --path . tests/water/HydroReferenceTests.tscn
godot --headless --path . tests/water/Phase1WaterSmoke.tscn
godot --path . tests/water/FixedHydroGPUSmoke.tscn
godot --path . tests/water/HydroGPUParityTests.tscn
godot --path . tests/water/HydroSurfaceReconstructionSmoke.tscn
```

Use the executable name/path appropriate for the local Godot 4.7 build.

## Remaining Phase 2 gates

1. Run all renderer-mode tests on the project GPU and fix numerical/API failures;
   do not hide real parity errors by broadly increasing tolerances.
2. Add a long closed-basin GPU soak test that samples only the four-byte volume
   diagnostic and records accumulated mass drift over many thousands of advances.
3. Exercise reconstructed dynamics in the normal playable world at a coast and
   check visual phase/LOD stability while moving the camera/clipmap.
4. Once those gates pass, freeze the fixed-domain numerical contract and begin
   Phase 3 sparse tile allocation + hydrological active-frontier scheduling.
