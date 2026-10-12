+++
path = "/world/weather-lab/global-system-status-p8"
title = "Storm domain: measured status of phase 8 (edge artifact, hot iteration, continuous sky)"
kind = "concept"
status = "experimental"
summary = """
Phase 8 of the storm domain plan, defined after phase 7: the child-edge artifact fixed, the storm kernels hot-reloadable through the central watcher, and the procedural sky kept around the storm instead of a clear patch; with the measurements."""
keywords = ["relaxation edge", "periodic wrap", "hot reload", "engine shader", "influence map", "continuous sky", "storm.reload_shaders", "phase 8"]
sources = [
  "engine/weather_gpu/shaders/storm.hlsl",
  "engine/hot_reload/src/ChangeClassifier.cpp",
  "engine/studio_ui/src/StudioViewportStorm.cpp",
  "engine/studio_ui/include/orbit/studio_ui/StormDomain.hpp",
  "engine/celestial_clouds/src/CloudRenderer.cpp",
]
symbols = ["ChangeKind", "RequestShaderReload", "StormInfluence"]
invariants = [
  "The outer two cells of a child are held to the parent outright every step, in the GPU kernel and in the CPU nest alike; a partial weight there lets the periodic grid's wrapped inflow survive as a sheet of cloud along the inflow edge.",
  "Saving engine/weather_gpu/shaders/storm.hlsl never restarts Studio: the change kind engine-shader reloads the kernels in place and a failed compile keeps the running ones.",
  "Results are single runs on one machine (an RTX 4090) of one storm and one camera.",
]
related = ["/world/weather-lab/global-system-status-p7", "/world/weather-lab/gpu-port", "/world/weather-lab/global-system-phases", "/rendering/clouds"]
depends_on = ["/world/weather-lab/global-system-status-p7"]
verify = ["orbit_weather_gpu nestdiag --nx 128 --ny 128 --steps 150 --sponge 12", "edit engine/weather_gpu/shaders/storm.hlsl while Studio runs a storm and watch storm.status shader_reload", "ctest -R \"Orbit.HotReload|Orbit.StormDomain|Orbit.WeatherGpu\""]
verified = "906ed92"
+++

**P8 (2026-10-12): three items, all met.**

**1. The wisp was an artifact, and is fixed.** `orbit_weather_gpu nestdiag` showed condensate in the child's outermost column (0.002 to 0.009 g/kg of ice at 9 to 10.5 km) where the parent had none. The child's grid is periodic: at its inflow (west) edge the semi-Lagrangian trace draws in air from the east edge, which carries the parent's anvil, and a relaxation weight of about 0.7 only removed part of it. One cell thick it is invisible face-on, but along a 64 km line of sight it has an optical depth of order 10, which is the faint arc seen west of the storm. The outer two cells are now held to the parent outright (weight 1); the edge column reads 0.0000 at every level, the arc is gone from the image, and the GPU nest still agrees with the CPU nest (child temperature 1.1e-3 K, wind 1.2e-3 to 2.7e-3 m/s), because the CPU nest got the same change.

**2. The storm kernels are hot-reloadable through the central watcher.** New change kind `engine-shader` for `engine/weather_gpu/shaders/*.hlsl` (classifier test), published by the hot-iteration service like content, handled by a Studio handler that queues a reload for the render thread; `storm.reload_shaders` / `orbit_storm_reload_shaders` do the same on demand and `storm.status` reports `shader_reload` and `shader_reloads`.

| Live check, a nested storm running | Result |
| --- | --- |
| Edit that scales the extinction by 0.25, file saved | `ok storm.hlsl` after 0.4 s, same process, storm steps unchanged (150); picture changes by 2.03/255 mean (the storm is much fainter) |
| Then a broken edit | `failed: AdvectVelocity: Orbit failed to compile HLSL to SPIR-V` after 0.1 s; picture unchanged (0.055/255, noise); same process; reload count not incremented |
| Then the original restored | `ok` after 0.1 to 0.2 s; picture back to the original within 0.07/255 |

**3. A continuous sky.** The simulation has no shallow cumulus, so replacing every procedural cloud inside the domain left a clear patch around the storm. `ExportCloud` now also writes an influence map (column optical depth, a separable maximum over about 40 km, smoothed to 0..1; header word 10) and the domain mask is multiplied by it, so the procedural clouds stay wherever the simulation has no cloud nearby. On screen the cumulus field returns around the storm, which stands in an ordinary-looking sky. Cost: the Clouds pass 3.9 ms with the storm against 4.0 without (inside the harness spread); the phase 6 timewarp results are unchanged (first visit 0.54 s, other jumps 0.23 s, worst frame 10.1 ms).

- Not done: a hot module boundary for the C++ side of the solver (only the HLSL is reloaded in place), the cloud shaders still reload through the generation refresh (they are C++ string literals), two-way feedback and the adaptive child, the 222 m level, climate in the schedule, FP16. The influence map's 40 km radius and 0.03 to 0.5 thresholds are untuned choices.
