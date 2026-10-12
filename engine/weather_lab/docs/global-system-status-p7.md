+++
path = "/world/weather-lab/global-system-status-p7"
title = "Storm domain: measured status of phase 7 (the nest on the GPU and in the clouds)"
kind = "concept"
status = "experimental"
summary = """
Phase 7 of the storm domain plan, added after phase 6: the 4:1 child grid of the CPU nest as GPU kernels, driven by NestedGpuSolver, exported with the parent's clouds and drawn by the cloud renderer with the child's finer extinction; measured agreement with the CPU nest, cost, and the on-screen difference."""
keywords = ["nested gpu solver", "child grid", "relaxation band", "gpu nest", "500 m", "cloud renderer", "storm child level", "phase 7"]
sources = [
  "engine/weather_gpu/include/orbit/weather_gpu/NestedGpuSolver.hpp",
  "engine/weather_gpu/include/orbit/weather_gpu/GpuStormSolver.hpp",
  "engine/weather_gpu/src/NestedGpuSolver.cpp",
  "engine/weather_gpu/shaders/storm.hlsl",
  "engine/celestial_clouds/src/CloudRenderer.cpp",
]
symbols = ["NestedGpuSolver", "NestedGpuConfig", "ParentLink", "StormChildView"]
invariants = [
  "The child is one-way: the parent evolves exactly as it would alone, so a nested storm is the unnested storm with a finer picture in the middle, never a different storm. Two-way feedback is not on the GPU.",
  "The child takes a fixed number of steps per parent step (default 2), so a nest is reproducible and a checkpoint of both levels restores bit for bit; the CPU nest's adaptive child step is not reproduced.",
  "Results are single runs on one machine (an RTX 4090) of one storm and one camera.",
]
related = ["/world/weather-lab/global-system-status-p6", "/world/weather-lab/gpu-port", "/world/weather-lab/global-system-phases", "/rendering/clouds"]
depends_on = ["/world/weather-lab/global-system-status-p6"]
verify = ["ctest -R Orbit.WeatherGpu", "orbit_weather_gpu nest --nx 128 --ny 128 --cells 32 --steps 40", "storm.place with nested true and false at the same camera and time, compare the images"]
verified = "5b0e452"
+++

**P7 (2026-10-12): the nest on the GPU and in the clouds; first slice.** The plan had listed "nesting kernels on the GPU" and "multi-resolution in the render" as open items after phase 6; this phase does both. The gate: the GPU nest matches the CPU nest, the child's finer extinction shows on screen, the cost stays small, and the phase 6 timewarp and determinism results still hold.

What exists: kernels `RelaxBand` (pull the child's edge band toward the parent, or fill the whole child from it at the start) and `CopyState` (the parent's state at the start of its step), a 52-dword parameter block and a fourth buffer slot, `GpuStormSolver::Advance(steps, dt, link)` for a child, `NestedGpuSolver` (parent plus child, one checkpoint for both), a child level in the exported cloud buffer (header words 22 to 31), `StormSigma` in the cloud shader blending from the parent's value to the child's across the child's relaxation band, and a `nested` option (default on) for `storm.place`, `storm.auto`, MCP and the panel.

| Check | Result |
| --- | --- |
| GPU nest against the CPU nest (32x32x40 parent at 2 km, 2:1 child at 1 km, 40 steps, same step schedule) | parent: theta 1.4e-3 K, u 1.1e-3, w 1.2e-3 m/s; child: theta 1.0e-3 K, u 1.1e-3, w 2.3e-3 m/s, qv 7e-7 |
| Nest checkpoint | both levels restore bit for bit and continue identically (test) |
| Cost, 128x128x40 parent at 2 km + 128x128 child at 500 m, 2 child steps per parent step | 2.01 ms per parent step (the parent alone is 0.61 ms), 11900x real time, 148 MiB |
| Cost, 256x256 parent + 256x256 child at 500 m | 8.5 ms per parent step, 2830x real time, 592 MiB |
| On screen, the same storm placed with and without the child, same camera and time | the child's picture has a crisper tiered anvil and a sharper shelf (mean image difference 0.38/255, 21500 pixels over 12/255 inside the storm); the parent's storm is the same in both |
| Viewport frame time | 3.8 to 5.6 ms with the nest, about the same as without |
| Phase 6 again with nested storms | first visit to an 82 minute storm 0.54 s, other jumps 0.23 s, worst frame 11.0 ms; two launches give the same `storm_id` and images 0.049/255 apart |
| Checkpoint memory | 50 MiB per nested checkpoint at 128x128 (25 per level); 5 checkpoints 251 MiB of the 640 MiB budget |

- An unresolved observation: with the child on, one frame shows a faint isolated arc of cloud in clear air to the west of the storm that the unnested storm does not have. It may be a simulated gust-front cloud, or an artifact at the child's edge; it has not been isolated.
- Not done: two-way feedback and the adaptive child (the child always sits over the middle quarter of the domain, which holds while the storm stays centred), an L2 level at 222 m, vertical stretching, climate in the schedule, wiring `ReloadShaders` to the central watcher, FP16.
