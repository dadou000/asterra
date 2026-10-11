+++
path = "/world/weather-lab/global-system-plan"
title = "Global weather system plan: tiers, layers and the role of CM1"
kind = "concept"
status = "experimental"
summary = """
Plan (not built) for replacing the analytic planetary weather map with a simulated, deterministic, timewarp-safe global system: what exists today, \
what CM1 can and cannot be inside a game engine, the tier stack with a shared state vector, the cloud layers derived from it, the phases with their \
measurable gates, and the decisions that are still open."""
keywords = ["global weather", "cm1", "tiers", "nesting", "cloud layers", "timewarp", "checkpoint", "fast core", "gpu port", "plan", "harmony"]
sources = [
  "engine/celestial_clouds/include/orbit/celestial_clouds/WeatherModel.hpp",
  "engine/celestial_clouds/include/orbit/celestial_clouds/CloudField.hpp",
  "engine/weather_lab/include/orbit/weather_lab/FastStormSolver.hpp",
  "engine/weather_lab/include/orbit/weather_lab/WxFormat.hpp",
  "tools/weather_lab/cm1_lab.py",
]
symbols = ["EvaluateWeather", "WeatherParameters", "WeatherState", "FastStormSolver", "CloudFieldProduct"]
invariants = [
  "Every tier reads and writes the same state variables (theta, qv, qc, qi, qr, u, v, w, p', aerosol, charge) so a handoff between tiers is interpolation, never a translation between models.",
  "Weather is a pure function of absolute simulation time plus stored checkpoints: the same time gives the same sky in two launches, and timewarp jumps to a checkpoint and re-spins, it never accumulates a backlog.",
  "CM1 is the validation oracle and an offline bake source; it is not the runtime solver. A global cloud-resolving grid does not fit (see the numbers in this block).",
  "LOD cannot create or destroy clouds: the renderer's cloud layers are derived from the solver state, and the analytic WeatherModel stays only as the initial condition and the fallback.",
]
related = ["/world/weather-lab/global-system-phases", "/world/weather-lab", "/world/weather-lab/sc01", "/rendering/unified-transport", "/rendering/clouds"]
depends_on = ["/world/weather-lab"]
verify = [
  "ctest -R Orbit.WeatherLab (passes on Windows, 2026-10-11, 8 s)",
  "Phase gates below are the acceptance tests; none has run yet except the baseline row.",
]
verified = "58f30fb"
+++

## What exists

| Piece | State | Where |
| --- | --- | --- |
| Analytic planetary weather | Built, used by the renderer. A deterministic function of direction and time (three-cell circulation, ITCZ, storm-track baroclinic waves, climate modulation): coverage, cloud type, cirrus, precipitation. Not a fluid solver. | `engine/celestial_clouds/.../WeatherModel.hpp` |
| Cloud field and renderer | Built. One GPU `CloudField` per body (type axis texels) feeds the globe composite, the ray-marched shell and cloud shadows. | `CloudField.hpp`, `CloudRenderer` |
| Fast storm core | Built, CPU reference. Moist anelastic, semi-Lagrangian, FFT pressure projection, Kessler warm rain; 291x real time at 64x64x40 on 4 threads, about 80 B per cell. | `engine/weather_lab` |
| CM1 reference harness | Built, offline. Stock supercell at 2 km, 1 km, 500 m, exported to `.orbitwx`; CM1 is 77x real time on the same 128 km box. | `tools/weather_lab/cm1_lab.py` |
| Weather lab panel and RPC | Built; library and RPC tests pass on Windows (`Orbit.WeatherLab`, `Orbit.WeatherLabRpc`, 2026-10-11). A visual check of the panel is still owed. | `engine/studio_ui/.../WeatherLab*` |
| Weather pre-spin | A workaround: the first scene after launch needs about 45 s of spin-up. | see `plan-and-validation` scenario 13 |

Not on this machine (checked 2026-10-11): a CM1 checkout, gfortran, MPI or NetCDF; the only WSL distribution is `docker-desktop`, which has no shell tools. The CM1 side of SC-01 can be read from the stored results but not re-run here yet.

## What CM1 can be

CM1 is a Fortran, MPI/OpenMP, cloud-resolving model on a Cartesian grid with periodic or open lateral boundaries. It has no spherical geometry. Counting cells shows why it cannot be the runtime global model:

- Earth's surface is about 5.1e8 km2. At 1 km that is 5.1e8 columns; with 40 levels, 2e10 cells. At the fast core's 80 B per cell that is about 1.6 TB, before any acoustic sub-stepping.
- CM1 itself ran 93.7 s of wall time for 120 simulated minutes of a 128 km square on 4 cores, which is 77x real time for 0.0003 of the globe.

So CM1 has three roles, in this order of value:

1. **Oracle.** Every fast-core change is compared against CM1 with `StormMetrics` (updraft, helicity, cold pool, cloud top). This exists; it needs a machine that can build CM1.
2. **Bake source.** Offline CM1 runs (a cluster, or Docker) produce `.orbitwx` storm libraries that the engine plays back through the same volume renderer as live solver output, for cases the fast core cannot yet do (ice, tornado-scale vorticity).
3. **Not the runtime.** A hosted CM1 process at runtime is rejected: process boundary, Fortran build chain, and no sphere. Porting CM1's numerics into C++ is only worth doing for a specific missing scheme (for example Morrison ice), and its distribution terms must be checked before any CM1 code is copied into this repository; running it offline needs no redistribution.

## Tier stack

| Tier | Scale | Method | Role |
| --- | --- | --- | --- |
| T0 climate | planet, seasons | existing climate fields (humidity, precipitation, temperature) | forcing and initial condition |
| T1 global | 50 to 100 km, cubed sphere | moist primitive-equation core on the GPU; the analytic `EvaluateWeather` is the first initial condition | fronts, jets, convergence zones, where storms are allowed |
| T2 mesoscale nest | 4 to 10 km, limited area | the fast core's dynamics with open boundaries, one-way nested in T1 | squall lines, fronts, orographic cloud |
| T3 storm nest | 0.5 to 2 km | the fast core on the GPU, spawned on a convective trigger from T2, frame moving with the storm | supercells, anvils, cold pools, lightning input |
| T4 detail | metres | the renderer's cloud genera and noise | what the camera sees; never feeds back |

All tiers share one state vector (invariant 1), so a nest is initialised by interpolating the parent and returns its mean tendencies to the parent when it ends. A nest that ends leaves a decaying sub-grid statistic in the parent, so a storm does not vanish when it stops being simulated.

## Cloud layers from the state

The renderer's layers are diagnosed, not authored: low (stratus, stratocumulus, cumulus), middle (altostratus, altocumulus), high (cirrus, cirrostratus) and convective towers plus anvil. The diagnosis is a column reduction of the solver's condensate (qc, qi, qr) and humidity to the `CloudField` type axis, coverage and density, plus precipitation and optical depth for the medium. The analytic `WeatherState` fields (coverage, cloudType, cirrus, precipitation) are exactly the interface the solver output has to fill, so the renderer does not change.

Timewarp, hot iteration, the phases with their gates and the open decisions are in `global-system-phases`.
