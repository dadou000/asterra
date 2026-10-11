+++
path = "/world/weather-lab/global-system-status-p6"
title = "Storm domain: measured status of phase 6 (placement and timewarp)"
kind = "concept"
status = "experimental"
summary = """
Phase 6 of the storm domain plan: scheduled storms as a pure function of the absolute simulation time, bit-exact solver checkpoints, storms that fade into the procedural clouds, and the measured timewarp and two-launch results."""
keywords = ["timewarp", "storm schedule", "checkpoint", "determinism", "fade", "scenario 13", "storm.auto", "bit exact"]
sources = [
  "engine/celestial_clouds/include/orbit/celestial_clouds/StormSchedule.hpp",
  "engine/celestial_clouds/src/StormSchedule.cpp",
  "engine/weather_gpu/include/orbit/weather_gpu/GpuStormSolver.hpp",
  "engine/studio_ui/src/StudioViewportStorm.cpp",
]
symbols = ["ScheduleStorm", "StormsNear", "ScheduledStorm", "StormCheckpoint", "StormAutoRequest"]
invariants = [
  "A scheduled storm is a pure function of (tile, slot, seed, weather parameters at the slot): it never depends on the camera or on what was simulated before, so every launch and every viewpoint agrees on which storms exist.",
  "Restoring a checkpoint and stepping on gives bit for bit the state of the run that never stopped (tested), so a storm is recomputed after any time jump rather than remembered across it.",
  "Results are single runs on one machine (an RTX 4090) of one camera and one season; the harness's own run-to-run noise is about 0.03/255 mean.",
]
related = ["/world/weather-lab/global-system-status-late", "/world/weather-lab/global-system-phases", "/world/weather-lab/gpu-port", "/rendering/clouds"]
depends_on = ["/world/weather-lab/global-system-status-late"]
verify = ["ctest -R \"Orbit.StormSchedule|Orbit.WeatherGpu|Orbit.StormDomain\"", "storm.auto then time.set to a time with a live storm near the camera; see /tools/ab-capture for the capture harness"]
verified = "fce65cd"
+++

**P6 (2026-10-12), first slice: scheduled storms and checkpointed timewarp; gate met on determinism and timewarp cost, the schedule itself is simple.**

What exists: `StormSchedule` (a deterministic hash over fixed 4 degree lat/lon tiles and six hour slots, weighted by the analytic weather map's convective score at the tile centre; no storms beyond 60 degrees latitude), `GpuStormSolver::Save/Restore` (bit-exact), a storm manager in the viewport renderer that follows the clock by storm age, caches a checkpoint every 20 storm minutes (384 MiB budget, least recently used storm dropped first), shows nothing while catching up and fades the storm over its last 40 minutes through a header intensity that multiplies the domain mask. Controls: `storm.auto` (RPC, MCP `orbit_storm_auto`, Weather Lab panel > Storm domain).

| Check | Result |
| --- | --- |
| Two independent solvers, same state | identical bit for bit after 40 steps (test `Orbit.WeatherGpu`) |
| Restore a checkpoint, step 25, against never stopping | identical bit for bit, also into a solver that never ran (test) |
| Schedule determinism | the same tile and slot give the same storm; a slightly different viewpoint sees the same storm and identity; queries 30 and 300 years ahead repeat exactly (test `Orbit.StormSchedule`) |
| Two launches at the same absolute time, camera aimed at the storm 323 km away | the same `storm_id` and age (81.8 min); images differ by 0.051/255 mean (the capture harness's own run-to-run noise is 0.02 to 0.07) |
| Storm lifecycle in one scan every 30 minutes | storms found near the camera at 4 to 6.5 h, 26 to 29.5 h and 32.5 to 36 h after the start; intensity 1.0 until the last 40 minutes, then 0.92, 0.41, 0.17, 0.02 |
| Timewarp from a storm at age 82 min | first visit ready in 0.34 s (6 frames), +20 min 0.23 s, +30 days and +10 years 0.23 s (no storm there, nothing drawn), back to the cached storm 0.23 s, to an earlier age 0.23 s; the worst frame in any jump is 11.4 ms, steady 6 to 8 ms |
| Cache | 5 checkpoints of this storm, 125 MiB (25 MiB each at 128x128x40) |

- The gate "Scenario 13 (extreme timewarp) passes without the 45 s pre-spin" holds for the simulated storm: no spin-up on launch, and a jump costs at most one checkpoint interval of steps (about 50) or, on a first visit to an old storm, one run from its start spread over frames. The procedural weather map needed no spin-up to begin with.
- Limits: the schedule only decides where storms start; the weather map's advected drift and the climate (ocean, desert) are not used (default climate), so tropical ocean and desert get the same odds; the season is quantised to 2 degrees; each storm differs only by its warm bubble (1.6 to 2.4 K); one storm at a time (the nearest); a storm is not carried across a body change; checkpoints live in memory only and are lost on exit; the first view of a 3 hour old storm needs about 450 steps (about 1.5 s of catch-up spread over frames).
