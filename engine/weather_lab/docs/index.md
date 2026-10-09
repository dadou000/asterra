+++
path = "/world/weather-lab"
title = "Weather lab (SC-01 storm experiments)"
kind = "subsystem"
status = "experimental"
summary = """
Storm experiments (SC-01): a CM1 harness that produces reference supercell data, the .orbitwx interchange format, a compressed 'fast core' \
storm solver (semi-Lagrangian anelastic with an exact FFT pressure solve and Kessler rain), storm metrics computed identically for both \
models, a headless WeatherLabSession, the orbit_weather_lab CLI, and the Studio 'Weather Lab' panel with its weather_lab.* RPC/MCP methods."""
owner_module = "OrbitWeatherLab"
keywords = ["weather", "supercell", "cm1", "storm", "anelastic", "semi-lagrangian", "orbitwx", "sc-01", "kessler", "fast core", "thunderstorm", "atmosphere simulation"]
sources = [
  "engine/weather_lab/include/orbit/weather_lab/FastStormSolver.hpp",
  "engine/weather_lab/include/orbit/weather_lab/Thermo.hpp",
  "engine/weather_lab/include/orbit/weather_lab/WxFormat.hpp",
  "engine/weather_lab/include/orbit/weather_lab/StormMetrics.hpp",
  "engine/weather_lab/src/FastStormSolver.cpp",
  "engine/weather_lab/src/Thermo.cpp",
  "engine/weather_lab/src/WxFormat.cpp",
  "engine/weather_lab/src/StormMetrics.cpp",
  "engine/weather_lab/include/orbit/weather_lab/WeatherLabSession.hpp",
  "engine/weather_lab/include/orbit/weather_lab/SliceColor.hpp",
  "engine/weather_lab/src/WeatherLabSession.cpp",
  "engine/studio_ui/include/orbit/studio_ui/WeatherLabRpc.hpp",
  "engine/studio_ui/include/orbit/studio_ui/WeatherLabUi.hpp",
  "engine/studio_ui/src/WeatherLabRpc.cpp",
  "engine/studio_ui/src/WeatherLabUi.cpp",
  "engine/weather_lab/tools/WeatherLabCli.cpp",
  "engine/weather_lab/CMakeLists.txt",
  "tools/weather_lab/cm1_lab.py",
]
symbols = ["FastStormSolver", "BuildSupercellBaseState", "WxReader", "WxWriter", "ComputeStormMetrics", "WeatherLabSession", "WeatherLabUi", "RegisterWeatherLabRpc", "SliceColor"]
applies_to = ["engine/weather_lab/**", "tools/weather_lab/**", "engine/studio_ui/src/WeatherLab*", "engine/studio_ui/include/orbit/studio_ui/WeatherLab*"]
invariants = [
  "Pure C++23 with no engine dependencies: the module configures, builds and tests on any host, so storm numerics can be verified without Studio or a GPU.",
  "nx and ny must be powers of two (FFT pressure solve); FastStormSolver::Validate reports it. The grid is doubly periodic with a rigid lid and floor, and a Galilean frame (frameU/frameV) keeps the storm near the domain centre.",
  "The pressure projection is a direct solve (2-D FFT + tridiagonal in z with density-weighted Neumann boundaries): divergence of rho0*u stays at round-off level (tested < 1e-6 1/s). It never iterates and has no acoustic modes.",
  "Semi-Lagrangian advection is not conservative; the mass fixer rescales qv/qc/qr after advection so total water (vapour + condensate + fallen rain) drifts < 1e-4 over a 25 minute storm (tested). Disabling massFixer is an experiment, not a default.",
  "MonotoneCubic advection is required for a persistent storm: Linear advection is too diffusive and the supercell dies (measured, 2 km, 120 min). Do not make Linear the default for speed.",
  "Results are deterministic and independent of the thread count: threads write disjoint ranges and reductions are sequential (tested 1 vs 4 threads).",
  ".orbitwx is the single interchange format for CM1 exports and fast-core output, so StormMetrics compares both models with the same definitions. Frame data is float32, x fastest then y then z, frame-major; truncated files keep their complete frames.",
  "WeatherLabSession is the one owner of the live run: the Weather Lab panel (WeatherLabUi) and every weather_lab.* RPC/MCP method call it, so a button and an agent do exactly the same thing. The panel owns no simulation logic; its view state (WeatherLabView) is edited by the panel and weather_lab.view, and any slice can be requested directly with weather_lab.slice.",
  "The solver steps on the session's own compute thread, never the UI or RPC thread. It publishes display snapshots about four times a second; GetSlice reads the last snapshot under a short lock, so queries never wait on a step. Settings are frozen while a live run exists (Configure fails; Reset first). The suite is ThreadSanitizer clean.",
  "Frame sampling lands exactly on the sample times (steps are shortened to hit them), so a recorded .orbitwx and a rerun with the same settings produce identical metrics (tested).",
  "Rain evaporation and fall speed use the SI forms from CM1's kessler.F. The Klemp-Wilhelmson constants in g/m^3 units under-evaporate by ~100x and removed the cold pool (measured 0.4 K vs CM1's 7 K); keep the SI forms.",
]
related = ["/world/weather-lab/sc01", "/world/fields", "/rendering/volumes"]
depends_on = []
used_by = ["/editor/studio-ui"]
verify = [
  "ctest -R Orbit.WeatherLab",
  "python tools/weather_lab/cm1_lab.py run --cm1 <CM1 checkout> --preset quick --out <dir> then orbit_weather_lab compare <dir>/cm1_quick.orbitwx <fast>.orbitwx",
]
verified = "f4a2dc6"

[routes]
"how the experiment was run, results, what is next" = "sc01"
+++

The weather lab exists to answer one question before any weather is integrated into Orbit: how much of a CM1 supercell survives
in a much cheaper solver? It has three parts.

**Reference.** `tools/weather_lab/cm1_lab.py` builds NCAR's CM1 from a checkout, runs the stock supercell case (Weisman-Klemp
sounding, quarter-circle hodograph, 1 K warm bubble, Morrison microphysics) at the `quick` (2 km), `standard` (1 km) or `fine`
(500 m) preset on a 128 km square domain, and exports the NetCDF output to `.orbitwx`.

**Fast core.** `FastStormSolver` is a moist anelastic model on a doubly periodic C grid. Each step is: semi-Lagrangian
advection (RK2 trace, monotone Catmull-Rom sampling) of u, v, w, theta, qv, qc, qr; a water-mass fixer; saturation adjustment
and Kessler autoconversion/accretion/evaporation; rain sedimentation (conservative upwind, subcycled); buoyancy, top sponge
and explicit mixing; then the exact pressure projection. Every stage is a plain loop over an array so it maps one-to-one to a
compute dispatch later.

**Comparison.** `StormMetrics` computes peak updraft/downdraft, cloud top, low-level vorticity, 2-5 km updraft helicity, cold pool
deficit and rain area from any `.orbitwx`. `orbit_weather_lab compare ref.orbitwx candidate.orbitwx` tabulates them side by side.

```
orbit_weather_lab run --minutes 120 --bubble 1 --dt 24 --courant 2.5 --out fast.orbitwx
orbit_weather_lab metrics fast.orbitwx
orbit_weather_lab compare cm1_quick.orbitwx fast.orbitwx
orbit_weather_lab budget            # resident bytes per cell
```

## Studio panel

Open **Weather Lab** from the Panels list (or `orbit_panel_focus("Weather Lab")`). Run / Pause / Step / Reset drive the live
fast-core run; Experiment settings configure it (Quick 2 km is the validated preset); Reference playback loads a CM1 `.orbitwx`
for side-by-side comparison; the plan map, x-z section and column-maximum canvases show w, rain, cloud water, vapour, theta
perturbation, vorticity, wind speed or total condensate; the metrics lines and the updraft history compare the live run with the
reference. Every control has a `weather_lab.*` method (`status`, `configure`, `control`, `playback`, `metrics`, `compare`,
`slice`, `view`) and an `orbit_weather_lab_*` MCP tool (docs/ORBIT_MCP.md).

## Hot iteration

- Saving a solver, session or panel source (.cpp/.hpp under engine/weather_lab or engine/studio_ui) is native code in a fast
  module with no reload boundary yet, so the central classifier takes the automatic native/generation fallback. The live run
  lives in the old generation's memory and does not survive the handoff; because runs are deterministic, `Reset` + `Start`
  (or the recorded `.orbitwx`) reproduces it. A failed build leaves the running generation, and its run, untouched.
- Saving a `.orbitwx` or other data file needs no rebuild: `weather_lab.playback load` re-reads it.
- Saving `tools/weather_lab/cm1_lab.py` needs nothing from Studio; it is an offline harness.
- There are no shaders or GPU resources yet (the panel draws with CPU canvases), so nothing needs deferred GPU retirement. The
  Vulkan port of the solver stages will need to follow `docs/ORBIT_HOT_ITERATION.md` (versioned module, deferred retirement).

## Diagnose

- Storm dies or never starts: check `--advection` (must be cubic), `--bubble`, and that `courant` is not so large that the
  trace leaves the storm (2.5 worked, 12+ s steps at 2 km are stable).
- Cold pool far weaker than CM1: rain evaporation units (see invariants).
- `maxDivergence` above 1e-6: the projection is not exact; check `nx`/`ny` powers of two and that w boundaries are zero.
- Water drift growing: `massFixer` is off, or sedimentation CFL (`0.9*dz`) was changed.
