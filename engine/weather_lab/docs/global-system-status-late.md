+++
path = "/world/weather-lab/global-system-status-late"
title = "Storm domain: measured status of phases 3 to 5"
kind = "concept"
status = "experimental"
summary = """
Measured results of the 512 km domain with lateral sponge, storm-following frame, adaptive child and cell budget over 6 simulated hours (P3), and the first slice of the GPU port (P4: agreement with the CPU reference and speed)."""
keywords = ["lateral sponge", "adaptive nest", "gpu port", "benchmark", "gate", "results", "storm domain", "512 km"]
sources = [
  "engine/weather_lab/include/orbit/weather_lab/NestedStormSolver.hpp",
  "engine/weather_gpu/include/orbit/weather_gpu/GpuStormSolver.hpp",
  "engine/weather_gpu/shaders/storm.hlsl",
]
symbols = ["NestedStormSolver", "GpuStormSolver"]
invariants = [
  "Results here are single runs of one case on one machine (an RTX 4090 for P4); storm metrics swing +-20% between cycles, so judge trends, not single rows.",
]
related = ["/world/weather-lab/global-system-status", "/world/weather-lab/gpu-port", "/world/weather-lab/global-system-phases"]
depends_on = ["/world/weather-lab/global-system-status"]
verify = ["orbit_weather_lab nest --nx 256 --ny 256 --dx 2000 --minutes 360 --ice 1 --sponge 16 --track 1 --adaptive 1", "orbit_weather_gpu bench --nx 256 --ny 256 --steps 40 --dt 24 --ice 1 --sponge 16"]
verified = "d4cfd08"
+++

**P3 (2026-10-12): mostly met; FP16 and L2 deferred.** Added to the fast core and the nest: a lateral sponge (`lateralSpongeCells`, `lateralSpongeRate`: relaxation to the environment within N cells of every edge, applied before the pressure projection), a storm-following Galilean frame (`trackStorm`), an adaptive child (`NestConfig::adaptive`: created at the main updraft when the parent peak reaches 8 m/s, released after the peak stays under 3 m/s for 15 minutes, extent halved when `maxChildCells` would be exceeded), and an activity mask in the microphysics (clear unsaturated cells are skipped).

Run: 512 km parent at 2 km (256x256x40), ice, sponge 16 cells, tracking, adaptive 64 km child at 500 m, 360 minutes, 8 threads:

| Measure | Result |
| --- | --- |
| Wall time | 749 s for 21600 s: 28.8x real time on the CPU (parent alone, periodic or sponge: 47x) |
| Cells | 2.62 M parent + 0.66 M child = 3.28 M (the plan's 4.4 M included the 222 m L2 level, not built) |
| Child | created once, at about 30 minutes, when the updraft reached 8 m/s; persisted to 360 minutes |
| Storm | peak updraft 44 to 55 m/s through the 6 hours, no decay |
| Divergence after projection | at most 1.4e-8 /s throughout (before the sponge was moved ahead of the projection it grew to 3e-5) |
| Outflow wrap | a 2 K warm pulse sent out of the east edge returns at the west edge in a periodic domain (0.075 K) and is absorbed with the sponge (0.0004 K), unit test `TestOutflowDoesNotWrap` |
| Budget cap | unit-tested (a 32 x 32 child is halved to 16 x 16 under a 20 000 cell cap); the 6 hour run's 655 k child was under its 700 k cap, so the cap did not bind there |

- The storm-following frame is rate limited to 1.5 m/s per 5 minutes after a first version let the frame jump to (-10, 28) m/s when the storm split. It still wanders between about (9, -2) and (27, 8) m/s over 6 hours, which is the storm's own motion changing; the storm stays alive and the child with it.
- A diagnostic of "condensate in the upstream quarter" cannot tell wrapping from the storm's own drift (sponge and periodic runs read alike), so the wrap evidence is the pulse test, not the full-storm runs.
- A fixed 12.5, 3.0 frame with the sponge keeps the same storm strength as the periodic run (peak updraft 45 to 51 m/s against 39 to 52 m/s).
- Not done: FP16 storage (deferred to the GPU port, where memory matters), vertical stretching, the 222 m level, more than one child, a child that moves relative to its parent.

**P4 (2026-10-12), first slice: single level on the GPU, gate met on speed and agreement.** `engine/weather_gpu` (`GpuStormSolver`, 14 HLSL compute kernels in `shaders/storm.hlsl`, card `/world/weather-lab/gpu-port`): advection, mass fixer, Kessler plus ice microphysics, buoyancy, mixing, top and lateral sponges, and the exact FFT pressure projection in float32. Measured on an RTX 4090:

| Check | Result |
| --- | --- |
| After 1 step, 32^3 grid | CPU and GPU agree to 1e-5 (u, v, w, theta); projection divergence 1.1e-9 on both |
| After 240 steps (48 min, mature storm, 64x64x40, ice) | mean field difference 1e-4 to 1e-3 of the field size; maxima 0.7 m/s in u and w (of 50) from the storm's chaos; divergence 1.3e-8 GPU, 7.9e-9 CPU |
| 256x256x40 against the CPU for 120 steps (48 min) | same: mean differences about 1e-4, divergence 1.4e-8 |
| Speed, 512 km domain (2.62 M cells), ice, sponge 16, fixed step 24 s | 2.68 ms per step: 8900x real time, about 180x the CPU (475 ms), against the gate's 10x |
| Memory | 231 MiB resident for 2.62 M cells with ice (88 B per cell including both ping-pong copies and the spectrum) |
| Reload | a shader that does not compile returns the error and the running pipelines keep working (test) |

- Not in this slice: the relaxation band and feedback kernels (the nest on the GPU), the adaptive step (a Courant limit needs a readback per step; the step is fixed), the storm-following frame, FP16 storage, wiring into the Weather Lab session and the central hot-reload watcher, RPC/MCP selection of the GPU solver.
- The speed figure is the solver step alone; the renderer's cloud pass and the readback to feed it are not included, and the cloud renderer shares the GPU.

**P5 (2026-10-12), first slice: the simulated storm is drawn in the viewport clouds; gate partly met.** `storm.place` (RPC, MCP `orbit_storm_place`, Weather Lab panel > Storm domain) builds a `GpuStormSolver` on the render thread, spins it up and steps it with the simulation clock; `GpuStormSolver::ExportCloud` writes extinction and a placement header; `CloudRenderer::SetStormBuffer` makes the march, the cloud shadow and the sun light volume use it (details: `/rendering/clouds/raymarch`).

| Check | Result |
| --- | --- |
| On screen, 128 cells at 2 km (256 km), 60 minute spin-up, storm 60 km north of the camera | a cumulonimbus with an overshooting top and an anvil sheared downwind, all from the simulated condensate; the procedural cumulus inside the domain is gone, the horizon cumulus beyond it is unchanged |
| Split view, simulated left of the divider, `CLOUD_STORM=0` right | same camera, time and pixels; the procedural half has small cumulus and no storm |
| No storm placed | images unchanged (0.04/255, run-to-run level); Clouds pass unchanged within the +-5% spread |
| Cost with a storm | the Clouds pass is about 3 ms in that view with or without; solver 60 MiB for 128x128x40; the spin-up (150 steps) built in one frame |
| Tests | `Orbit.StormDomain` (mailbox and RPC), `Orbit.WeatherGpu` (export header and extinction range) |

- The gate's "no visible seam at the domain edge" is not shown: the edge is a fade over the sponge width, the view looked continuous at the horizon, but only one camera and one time were checked, and the simulation has no shallow cumulus so the domain is a clear patch around the storm. Relaxing the simulation's boundary toward the procedural weather would remove the patch.
- Not done: the storm follows the simulation clock but a jump over an hour restarts it (checkpoints are phase 6); only one domain; the solver submits and waits on the render thread (at most 8 steps per frame, about 3 ms each); no nesting on the GPU yet, so the viewport storm is a single 2 km level; silhouette detail is erosion noise, not simulated.

