+++
path = "/world/weather-lab/global-system-status-p10"
title = "Storm domain: measured status of phase 10 (rain shafts and the storm's own lightning)"
kind = "concept"
status = "experimental"
summary = """
Phase 10 of the storm domain plan: the rain becomes its own channel in the cloud export so the renderer can break it into shafts, and the simulated storm's graupel, ice, supercooled water and updraft charge a window of its grid and fire flashes (storm_electrification on a worker thread) into the transient flash registry. Slow-motion and instability levers for testing; what was seen and what was not verified."""
keywords = ["rain shafts", "rain channel", "storm lightning", "electrification", "slow motion", "charge boost", "breakdown scale", "storm.lightning", "phase 10"]
sources = [
  "engine/studio_ui/src/StudioViewportStorm.cpp",
  "engine/studio_ui/src/StormDomain.cpp",
  "engine/weather_gpu/shaders/storm.hlsl",
  "engine/celestial_clouds/src/CloudRenderer.cpp",
  "engine/storm_electrification/include/orbit/storm_electrification/StormLightningSource.hpp",
]
symbols = ["StormLightningSettings", "StormLightningFeed", "StormLightningSource"]
invariants = [
  "The rain part of the extinction is a second region of the cloud buffer (header word 11, parent cells then child cells), written by the same ExportCloud kernel with arg0 = 1; it never exceeds the whole extinction of its cell, and the shafts only reshape it (the mean stays about 1), so the far view keeps the simulated amount.",
  "The flash graphs from the electrification carry pulse times on the electrification clock; the feed shifts them to start at zero before they reach the registry, otherwise every later flash is delayed by the time the source has run.",
  "The electrification runs on a worker thread over a 64 x 64 window of the parent grid centred on the ice at the first read; the GPU state is read back on the render thread only when the worker is idle and 10 storm seconds are pending (about 9 ms).",
  "slow_motion, charge_boost and breakdown_scale are testing levers (storm.lightning): slow motion stretches a flash and multiplies its energy by the same factor, so its power is unchanged; changing the boost or the breakdown restarts the charge.",
  "Results are single runs of one storm on one machine; the flash lighting of the clouds was not captured in a screenshot (a flash lasts a fraction of a frame), it was seen live.",
]
related = ["/world/weather-lab/global-system-status-p9", "/world/weather-lab/gpu-port", "/rendering/unified-transport/storm-electrification"]
depends_on = ["/world/weather-lab/global-system-status-p9"]
verify = ["ctest -R \"Orbit.WeatherGpu|Orbit.StormDomain|Orbit.StormLightning\"", "storm.place, then storm.lightning with slow_motion 100, charge_boost 40, breakdown_scale 0.25 while time runs at rate 10 and Studio runs with ORBIT_CLOUD_POINT_LIGHTS=1: storm.status lightning_flashes grows"]
verified = "a73c5f3"
+++

**P10 (2026-10-12): the two follow-ups of phase 9, rain and lightning.**

**1. Rain as its own channel.** `ExportCloud` now also writes the rain's part of the extinction (3 q_r rho) into a second region of the cloud buffer, found through header word 11; the child's cells follow the parent's, as for the extinction. `StormSigma` reads both and breaks the rain into streaks (two octaves, 320 and 110 m wide, leaning with height, clear air between them, `CLOUD_STORM_SHAFTS_ON`, `CLOUD_STORM_SHAFTS`, `CLOUD_STORM_RAIN`). The shafts fade out as a pixel gets wider than the streaks. Result: the streaks are there and visible where the simulation has rain against a bright background (the edge of the rain shaft at 10 km), but at 60 minutes this storm has little rain near the surface and the wall seen from the south is cloud water, so the picture changes little; a multiplier of 5 on the rain made the streaks clearer without changing the rest. A flat dark base remains: nothing yet gives the cloud base structure of its own.

**2. Lightning from the storm's own fields.** A feed (`StormLightningFeed`, `StudioViewportStorm.cpp`) reads the parent state back every 10 storm seconds, crops a 64 x 64 window around the ice, builds `StormFields` (temperature from theta and the base Exner, density, updraft, graupel, ice + snow, cloud water) and runs `StormLightningSource` on a worker thread, with `UpdateFields` keeping the charge while the fields change. The flashes come back in grid coordinates and are rotated into the body-fixed frame at the storm's centre (east, north, up), then added to the transient flash registry, so they light the clouds when Studio runs with `ORBIT_CLOUD_POINT_LIGHTS=1`.

| Check | Result |
| --- | --- |
| Readback and field conversion | about 9 ms every 10 storm seconds on the render thread (3.4 ms GPU copy, 5 ms fields) |
| Cloud export per step, with the rain region | 0.5 ms; the storm step is 2.5 ms |
| Flashes, boosted (charge x40, breakdown x0.25) | 88 flashes in about 600 storm seconds, at the storm's position (planet-centred coordinates match the centre) |
| Flashes, default settings | 6 to 14 flashes in about 200 storm seconds once the charge had built for about 100 |
| Seen on screen | yes, live, with point lights on (the first flash was seen by the user while testing) |

Testing levers (`storm.lightning`, `orbit_storm_lightning`): `slow_motion` (1 to 2000) stretches every flash and keeps its power, `charge_boost` (1 to 10000) multiplies the charging, `breakdown_scale` (0.02 to 1) lowers the breakdown field, so a storm can be made electrically unstable on demand.

What the work found:
- The pulse times of a flash are absolute on the electrification clock; the first version added them as they were and each flash was delayed by the time the source had run (remaining lifetime 64 s at age 1.4 s). Shifted to start at zero they live 1 to 1.5 s. `StormLightningController` hands graphs to the registry without that shift; worth checking there.
- With simulation time running at rate 20 every storm step coincided with a frame of about 1 s (rate 5: one per step, and the same with the lightning off), unrelated to the exports (0.5 ms) and the readback (9 ms); not diagnosed here.

- Not done: a screenshot of a flash (a flash lasts a fraction of a frame; `viewport.screenshot` cannot catch it, and slow motion beyond the pulse length would need a frozen flash clock), electrification on the child grid, the charge window follows the ice only at its start, the surface lighting pass uses the same flashes but was not checked, cloud-base structure, mammatus.
