+++
path = "/world/weather-lab/sc01"
title = "SC-01 experiment log: CM1 versus the fast core"
kind = "reference"
status = "experimental"
summary = """
Measured results of running the stock CM1 supercell and the compressed fast core at 2 km, what matched, what did not, the speed \
difference on CPU, and the open questions (GPU port, AMR, ice, Studio integration)."""
owner_module = "OrbitWeatherLab"
keywords = ["sc-01", "experiment", "benchmark", "cm1", "fast core", "results", "speedup", "supercell", "cold pool", "updraft helicity"]
sources = [
  "tools/weather_lab/cm1_lab.py",
  "engine/weather_lab/tools/WeatherLabCli.cpp",
]
symbols = ["Compare"]
invariants = [
  "Comparisons are only valid between runs on the same domain: 128 km square, 20 km deep, WK sounding, Galilean frame (12.5, 3.0) m/s, warm bubble 1 K (CM1's hard-coded value), same output cadence.",
  "CM1 reference runs use the Morrison two-moment scheme with ice; the fast core is warm-rain only. Differences in cloud top and anvil are expected until ice is added; the cold pool and rotation are the fair comparison.",
]
related = ["/world/weather-lab"]
verify = ["Re-run the commands below on the same machine and compare the table; metrics fluctuate +-20% between storm cycles, so judge trends, not single rows."]
verified = "f4a2dc6"
+++

## Setup

CM1 r22.x from NCAR/cm1 (gfortran 13, OpenMP, NetCDF). Machine: 4 CPU cores, no GPU. Both models: 128 km x 128 km x 20 km,
dx = dy = 2 km, dz = 500 m, 120 simulated minutes, output every 10 minutes.

```
python tools/weather_lab/cm1_lab.py build --cm1 <cm1>
python tools/weather_lab/cm1_lab.py run   --cm1 <cm1> --preset quick --out cm1_quick --threads 4
orbit_weather_lab run --minutes 120 --bubble 1 --dt 24 --courant 2.5 --threads 4 --out fast.orbitwx
orbit_weather_lab compare cm1_quick/cm1_quick.orbitwx fast.orbitwx
```

## Results (2 km, 120 min)

| Measure | CM1 | Fast core (dt 24 s) |
| --- | --- | --- |
| Wall time, 4 threads | 93.7 s (77x real time) | 24.7 s (291x real time) |
| Steps | adaptive dt ~8 s, 4 acoustic sub-steps each | 300 (24 s) |
| Peak updraft, 60-120 min | 47-54 m/s | 32-46 m/s |
| Updraft helicity 2-5 km, 60-120 min | 368-564 | 244-574 |
| Cold pool deficit at 120 min | 7.3 K | 6.7 K |
| Low-level vorticity | 0.004 /s | 0.005 /s |
| Cloud top | 15-16 km | 12-14 km |
| Resident memory | n/a | 13 MiB for 64x64x40 (~80 B/cell, CPU reference) |
| Total water drift | n/a | < 4e-5 relative |

The fast core is about 3.8x faster than CM1 on the same grid, holds a persistent rotating storm with a realistic cold pool,
and keeps divergence at round-off. It is weaker in sustained peak updraft and lower in cloud top (no ice, no latent heat
of freezing).

## Findings that changed the design

- Linear (trilinear) semi-Lagrangian advection killed the storm; monotone cubic is required (cost: advection is ~70% of a step).
- The first Kessler evaporation used g/m^3 constants and produced a 0.4 K cold pool against CM1's 7 K; the SI form from CM1
  fixed it. Cold pool is the metric that exposes microphysics errors fastest.
- Time step: 12 s and 24 s give statistically similar storms; the semi-Lagrangian scheme does not need an advective CFL below 1.
- Speed is bounded by advection sampling, not the pressure solve (3-5 ms of a 70-80 ms step), which argues for the GPU port
  doing the cubic gathers with texture hardware or shared-memory tiles.

## Open

1. Add a simple ice/graupel scheme and latent heat of freezing; compare cloud top and updraft against `standard` (1 km) CM1.
2. Resolution: run fast core at 1 km and 500 m against the `standard` and `fine` presets; the question is whether 2 km hides
   tornado-scale vorticity (expected) and how much finer the fast core needs before UH converges.
3. GPU port of the stages (Vulkan compute) and a memory budget against the 2 GB weather allocation: ~80 B/cell on CPU means
   ~17 M cells would fit (70% of 2 GiB), before AMR.
4. Studio integration: volume-render `.orbitwx` through the existing volume renderer, run the fast core as a Simulation-workspace
   job, expose RPC/MCP (`weather_lab.*`) and a docs update for `docs/ORBIT_MCP.md`.
5. Open lateral boundaries (the domain is periodic, so outflow re-enters after ~2 h).
