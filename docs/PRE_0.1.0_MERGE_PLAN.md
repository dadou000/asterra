# Asterra pre-0.1.0 integration plan

Branch: `pre/0.1.0`

Base: `terrain/0.0.5-terrain-viewport-tools`

Goal: construct the complete Asterra 0.1.0 integration without modifying the existing subsystem branches.

## Source-of-truth branches

### World / terrain / rendering / character

Use `terrain/0.0.5-terrain-viewport-tools` as authoritative for:

- Planet Studio and world authoring
- terrain viewport tools and terrain presets
- authoritative GPU terrain / spherical clipmaps
- terrain shader graph and displacement runtime
- terrain deformation and scatter
- celestial bodies, orbital runtime, gas giants
- current rendering / graphics quality / exposure stack
- current character editor and procedural grooming
- current active-ragdoll / physics-walker runtime

### Weather / hydrology

Integrate from `water/0.1.0` as authoritative for:

- `native/weather/`
- `scripts/weather/`
- sparse and persistent hydrology under `scripts/water/`
- hydrology compute shaders under `shaders/water/`
- water/weather validation tests
- weather/hydrology build support

Do not blindly replace terrain/world-authoring files from the water branch. Where water/weather touches terrain, ocean rendering, rendering integration, `project.godot`, `world.tres`, `scripts/main.gd`, or `scripts/core/frames.gd`, reconcile the newer water hooks into the pre-0.1.0 terrain/world architecture.

## Selective experimental imports

### Vehicle/world integration probes

Source: `test/gpu-terrain-player-vehicles-wind-0.0.5`

Candidate files:

- `scripts/vehicles/vehicle_test_harness.gd`
- `scripts/vehicles/wind_vehicle_test.gd`

These are validation harnesses, not production vehicle physics.

### Neural locomotion experiment

Source: `experiment/19body-neural-walk-jolt`

Keep isolated under:

- `experiments/locomotion_19body/`

Do not replace the release player/runtime tree with the experiment branch.

## Historical branches

The following are reference/regression sources only unless a specific regression requires mining them:

- `terrain/0.0.5-gpu-graphics-pass1`
- `terrain/0.0.5-gpu-geomorph-pass1`
- `terrain/0.0.5-gpu-geomorph-latest-clipmaps`
- `terrain/0.0.5-simplified-terrain-ui`
- `terrain/0.0.5-authoritative-terrain-stack`
- `terrain/0.0.5-terrain-presets`
- `weather/0.0.4`
- `weather/0.0.5`
- older `integrate/*` branches
- older `water/integration-*` branches

## High-conflict integration files

Resolve manually rather than selecting an entire side:

- `project.godot`
- `world.tres`
- `scripts/main.gd`
- `scripts/core/frames.gd`
- `scripts/terrain/coastline_clipmap.gd`
- `scripts/terrain/ocean_geometry_clipmap.gd`
- `scripts/terrain/ocean_buoyancy_3d.gd`
- `scripts/terrain/ocean_gpu_physics.gd`
- `scripts/terrain/orbit_ocean.gd`
- `scripts/terrain/spherical_geometry_clipmap*.gd`
- `scripts/rendering/cloud_depth_compositor_effect.gd`
- `scripts/rendering/volumetric_cloud_controller.gd`
- `scripts/rendering/graphics_quality.gd`
- `shaders/ocean_geometry_clipmap.gdshader`
- `shaders/ocean_waves.gdshaderinc`
- `shaders/planet_lighting.gdshaderinc`

## Integration order

1. Preserve the `pre/0.1.0` terrain/world baseline.
2. Import isolated weather native sources and weather scripts.
3. Import isolated hydrology scripts, compute shaders, docs and tests.
4. Reconcile autoloads/configuration in `project.godot`.
5. Reconcile ocean/terrain/rendering bridge files manually.
6. Reconcile weather-to-water and terrain-to-water coupling.
7. Port vehicle/wind integration probes.
8. Preserve neural locomotion as a non-release-critical experiment.
9. Run terrain, Planet Studio, weather and water validation suites.
10. Only after the integrated branch is stable, promote it to `0.1.0`.
