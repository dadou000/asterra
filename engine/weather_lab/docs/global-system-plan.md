+++
path = "/world/weather-lab/global-system-plan"
title = "Storm domain plan: a 500 km CM1-class domain with adaptive resolution"
kind = "concept"
status = "experimental"
summary = """
Plan (not built) for the weather system the engine actually runs: one storm domain up to about 500 km across, simulated CM1-style with aggressive multi-resolution \
(a coarse base grid, finer nests only where the storm is, vertical stretching, activity masks) so it stays cheap, and feeding the existing cloud renderer. Covers \
what exists, the cell budget, the optimisations, the pressure-solve risk across resolution levels, the cloud layers, and CM1's role as oracle and bake source."""
keywords = ["storm domain", "nesting", "adaptive resolution", "amr", "cm1", "cloud layers", "fast core", "gpu port", "500 km", "multi resolution", "plan"]
sources = [
  "engine/celestial_clouds/include/orbit/celestial_clouds/WeatherModel.hpp",
  "engine/celestial_clouds/include/orbit/celestial_clouds/CloudField.hpp",
  "engine/weather_lab/include/orbit/weather_lab/FastStormSolver.hpp",
  "engine/weather_lab/include/orbit/weather_lab/WxFormat.hpp",
  "tools/weather_lab/cm1_lab.py",
]
symbols = ["EvaluateWeather", "WeatherParameters", "WeatherState", "FastStormSolver", "CloudFieldProduct"]
invariants = [
  "The simulated domain is a limited area (up to about 500 km square, 20 km deep) placed on the planet; outside it the analytic WeatherModel supplies the boundary and the sky. There is no global solver in this plan.",
  "Every resolution level reads and writes the same state variables (theta, qv, qc, qi, qr, u, v, w, p', aerosol, charge), so a nest is initialised by interpolation and returns its tendencies by averaging, never by translation between models.",
  "Cost is bounded by construction: refinement is limited to a cell budget, and when the budget is exceeded the finest level coarsens instead of the frame time growing.",
  "CM1 is the validation oracle and an offline bake source; it is not the runtime solver.",
  "LOD cannot create or destroy clouds: the renderer's cloud layers are derived from the solver state; the analytic map is the initial condition, the boundary and the fallback.",
]
related = ["/world/weather-lab/global-system-phases", "/world/weather-lab", "/world/weather-lab/sc01", "/rendering/unified-transport", "/rendering/clouds"]
depends_on = ["/world/weather-lab"]
verify = [
  "ctest -R Orbit.WeatherLab (passes on Windows, 2026-10-11, 8 s)",
  "The cell budget below is arithmetic from the fast core's measured 80 B per cell, not a measurement of a nested run; the phase gates are the acceptance tests.",
]
verified = "58f30fb"
+++

## What exists

| Piece | State | Where |
| --- | --- | --- |
| Analytic planetary weather | Built, used by the renderer: a deterministic function of direction and time (circulation cells, ITCZ, storm-track waves, climate modulation) giving coverage, cloud type, cirrus, precipitation. | `engine/celestial_clouds/.../WeatherModel.hpp` |
| Cloud field and renderer | Built. One GPU `CloudFieldProduct` per body (type-axis texels) feeds the globe composite, the ray-marched shell and cloud shadows. | `CloudField.hpp`, `CloudRenderer` |
| Fast storm core | Built, CPU, uniform grid. Moist anelastic, semi-Lagrangian, exact FFT pressure projection, Kessler warm rain; 291x real time at 64x64x40 on 4 threads, about 80 B per cell. | `engine/weather_lab` |
| CM1 reference harness | Built, offline: stock supercell at 2 km, 1 km, 500 m, exported to `.orbitwx`. CM1 ran 77x real time on the same 128 km box. | `tools/weather_lab/cm1_lab.py` |
| Weather Lab panel and RPC | Built; library and RPC tests pass on Windows (2026-10-11). A visual check of the panel is owed. | `engine/studio_ui/.../WeatherLab*` |

Not on this machine (2026-10-11): a CM1 checkout, gfortran, MPI or NetCDF; the only WSL distribution is `docker-desktop`. SC-01's CM1 side can be read, not re-run, until CM1 builds here.

## The domain and its cell budget

One storm domain, up to 500 km x 500 km x 20 km, translating with the storm (Galilean frame, as the fast core does now). Uniform 222 m over that area would be 2250 x 2250 x 80 = 405 M cells, about 32 GB at 80 B per cell: impossible. Nesting puts the resolution where the physics is:

| Level | Spacing | Area | Levels (dz) | Cells | At 80 B/cell |
| --- | --- | --- | --- | --- | --- |
| L0 base | 2 km | 500 x 500 km | 40 (500 m, stretched above 12 km) | 250 x 250 x 40 = 2.5 M | 200 MB |
| L1 | 667 m (3:1) | 100 x 100 km around the main storm | 40 | 150 x 150 x 40 = 0.9 M | 72 MB |
| L2 | 222 m (3:1) | 25 x 25 km around the updraft and mesocyclone | 80 (250 m) | 112 x 112 x 80 = 1.0 M | 80 MB |
| Total | | | | about 4.4 M | about 350 MB |

That is about 90 times fewer cells than uniform fine, inside the 2 GB weather budget with room for a second storm. The fast core did 164 k cells at 291x real time on 4 CPU threads; 4.4 M cells is 27 times more, so a CPU run would be roughly 10x real time before sub-cycling the fine levels, and the GPU port has the headroom. These are estimates from arithmetic; gate P3 measures them.

## Optimisations (in order of expected payoff)

1. **Nest only where it matters.** Refinement follows an indicator (updraft above a threshold, vertical vorticity, condensate gradient) with hysteresis, snapped to coarse-cell boundaries, with a cell budget cap (invariant 3). L2 exists only while a mesocyclone does.
2. **Vertical stretching.** 500 m to 12 km, then stretched to the lid; fine only in the boundary layer where it matters (125 to 250 m below 2 km for the cold pool).
3. **Semi-Lagrangian time steps.** The fast core's step does not depend on advective Courant number; each level takes its own step (24 s, 8 s, 3 s), so the fine levels sub-cycle only 3x per parent step, not 9x.
4. **Activity masks.** Microphysics, saturation adjustment and sedimentation run only in cells with condensate or supersaturation; most of a 500 km domain is clear air. Brick (4x4x4) occupancy decides which bricks dispatch.
5. **Reduced storage.** FP16 for qv/qc/qr and the rain-free fields, FP32 for theta and the pressure solve; about 80 B to about 50 B per cell.
6. **Output on demand.** The renderer reads density bricks only for the cloud volume in view and a 2-D coverage product for the rest, so cloud rendering never pays for the full state.

## The pressure-solve risk across levels

The fast core's exact FFT projection needs a uniform periodic grid; a nest breaks that. This is the one real technical risk in the plan:

- L0 keeps the FFT solve (uniform; the sponge makes the periodic wrap harmless, and open outflow is phase P3's boundary work: at 500 km and storm speeds of about 20 m/s the outflow needs several hours to wrap, versus about 2 h at 128 km today).
- L1 and L2 solve pressure with a few geometric-multigrid cycles on the nest block, with Dirichlet data from the parent after interpolation, then correct the parent from the child average (two-way). Multigrid is also the GPU-friendly choice.
- Gate: a one-way 667 m nest inside a 2 km base must reproduce a uniform 667 m run over the same 100 km window (updraft helicity, cold pool, divergence of rho0 u at round-off inside each level) before anything else is built on it.

## Cloud layers from the state

The renderer's layers are diagnosed, not authored: low (stratus, stratocumulus, cumulus), middle, high (cirrus, anvil) and convective towers. The diagnosis reduces the solver's condensate (qc, qi, qr) and humidity per column to the `CloudFieldProduct` type axis, coverage and density, plus precipitation and optical depth. Inside the finest level the renderer reads 3-D density bricks directly (the volume renderer), so a mesocyclone's wall cloud and anvil are the simulated ones, not noise. Outside the domain, and as a fallback, the analytic `WeatherState` fields apply, and they are the same fields the solver output has to fill, so the renderer's interface does not change.

## What CM1 is

CM1 is a Fortran, MPI/OpenMP, cloud-resolving model on Cartesian grids. Its roles:

1. **Oracle.** Every fast-core and nesting change is compared against CM1 with `StormMetrics` (updraft, helicity, cold pool, cloud top, rain area). Needs a machine that can build CM1.
2. **Bake source.** Offline CM1 runs (a cluster, or Docker) produce `.orbitwx` storm libraries that play back through the same volume renderer, for what the fast core cannot yet do (ice, tornado-scale vorticity).
3. **Not the runtime.** Hosting CM1 as a process is rejected: process boundary, Fortran build chain, no nesting we control. Porting a specific scheme (for example Morrison ice) is allowed; CM1's distribution terms must be confirmed before copying any of its code.
