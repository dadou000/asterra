+++
path = "/world/weather-lab"
title = "Weather lab (SC-01 storm experiments)"
kind = "subsystem"
status = "experimental"
summary = """
Standalone atmosphere experiments: a CM1 harness that produces reference supercell data, the .orbitwx interchange format, a compressed \
'fast core' storm solver (semi-Lagrangian anelastic with an exact FFT pressure solve and Kessler rain), storm metrics computed identically \
for both models, and the orbit_weather_lab CLI. Not yet wired into Studio."""
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
  "engine/weather_lab/tools/WeatherLabCli.cpp",
  "engine/weather_lab/CMakeLists.txt",
  "tools/weather_lab/cm1_lab.py",
]
symbols = ["FastStormSolver", "BuildSupercellBaseState", "WxReader", "WxWriter", "ComputeStormMetrics"]
applies_to = ["engine/weather_lab/**", "tools/weather_lab/**"]
invariants = [
  "Pure C++23 with no engine dependencies: the module configures, builds and tests on any host, so storm numerics can be verified without Studio or a GPU.",
  "nx and ny must be powers of two (FFT pressure solve); FastStormSolver::Validate reports it. The grid is doubly periodic with a rigid lid and floor, and a Galilean frame (frameU/frameV) keeps the storm near the domain centre.",
  "The pressure projection is a direct solve (2-D FFT + tridiagonal in z with density-weighted Neumann boundaries): divergence of rho0*u stays at round-off level (tested < 1e-6 1/s). It never iterates and has no acoustic modes.",
  "Semi-Lagrangian advection is not conservative; the mass fixer rescales qv/qc/qr after advection so total water (vapour + condensate + fallen rain) drifts < 1e-4 over a 25 minute storm (tested). Disabling massFixer is an experiment, not a default.",
  "MonotoneCubic advection is required for a persistent storm: Linear advection is too diffusive and the supercell dies (measured, 2 km, 120 min). Do not make Linear the default for speed.",
  "Results are deterministic and independent of the thread count: threads write disjoint ranges and reductions are sequential (tested 1 vs 4 threads).",
  ".orbitwx is the single interchange format for CM1 exports and fast-core output, so StormMetrics compares both models with the same definitions. Frame data is float32, x fastest then y then z, frame-major; truncated files keep their complete frames.",
  "Rain evaporation and fall speed use the SI forms from CM1's kessler.F. The Klemp-Wilhelmson constants in g/m^3 units under-evaporate by ~100x and removed the cold pool (measured 0.4 K vs CM1's 7 K); keep the SI forms.",
]
related = ["/world/weather-lab/sc01", "/world/fields", "/rendering/volumes"]
depends_on = []
used_by = []
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

## Hot iteration

This is an offline lab, not a Studio runtime system yet, so there is no in-process reload path to violate. Editing the solver
means rebuilding the `OrbitWeatherLab` target and re-running the CLI (a few seconds). The Studio integration (volume
rendering of `.orbitwx` frames, a Simulation-workspace panel running the fast core as a job, and matching RPC/MCP methods) is
the next step and must follow `docs/ORBIT_HOT_ITERATION.md` and `docs/ORBIT_MCP.md` when added; until then there is nothing
reachable in Studio that lacks an RPC/MCP path.

## Diagnose

- Storm dies or never starts: check `--advection` (must be cubic), `--bubble`, and that `courant` is not so large that the
  trace leaves the storm (2.5 worked, 12+ s steps at 2 km are stable).
- Cold pool far weaker than CM1: rain evaporation units (see invariants).
- `maxDivergence` above 1e-6: the projection is not exact; check `nx`/`ny` powers of two and that w boundaries are zero.
- Water drift growing: `massFixer` is off, or sedimentation CFL (`0.9*dz`) was changed.
