+++
path = "/world/weather-lab/global-system-phases"
title = "Storm domain: phases with gates, time and determinism, hot iteration, open decisions"
kind = "concept"
status = "experimental"
summary = """
Second half of the storm domain plan: the phases with measurable gates (nesting before anything else), how a storm domain stays deterministic under timewarp, how the solver stays hot-reloadable, and the decisions still open."""
keywords = ["timewarp", "checkpoint", "hot reload", "phases", "gates", "global weather", "cm1", "gpu port", "nesting"]
sources = [
  "engine/weather_lab/include/orbit/weather_lab/FastStormSolver.hpp",
  "tools/weather_lab/cm1_lab.py",
]
symbols = ["FastStormSolver"]
invariants = [
  "A phase is done only when its gate is measured, not when the code exists; no gate has run yet.",
]
related = ["/world/weather-lab/global-system-plan", "/world/weather-lab/sc01", "/rendering/unified-transport/plan-and-validation"]
depends_on = ["/world/weather-lab/global-system-plan"]
verify = ["ctest -R Orbit.WeatherLab (baseline only)"]
verified = "58f30fb"
+++

## Phases and gates

Order follows risk: the nesting and the pressure solve across levels come before the GPU port, because the port would otherwise freeze an architecture that nesting may change.

| Phase | Work | Gate |
| --- | --- | --- |
| P1 reference | CM1 buildable here (Docker or a WSL distribution), SC-01 re-run on this machine; ice and graupel plus latent heat of freezing in the fast core | Cold pool 6 to 8 K, updraft helicity within CM1's band, cloud top 15 to 16 km at 2 km |
| P2 nesting on the CPU | A level structure in the fast core: a 667 m child inside a 2 km base, one-way, then two-way; multigrid pressure on the child with Dirichlet data from the parent; vertical stretching | The child reproduces a uniform 667 m run over the same 100 km window (helicity and cold pool within the +-20% cycle-to-cycle spread, divergence at round-off per level); cell count at least 8x lower |
| P3 domain and boundaries | 500 km base, open lateral boundaries, moving frame, refinement indicator with hysteresis and a cell budget, activity masks, FP16 storage | Outflow does not wrap in 6 simulated hours; the budget cap holds (the finest level coarsens instead of the step growing); total about 4.4 M cells measured |
| P4 GPU port | Vulkan compute port of the level stack as a hot module; the per-level steps and the multigrid on the GPU | Matches the CPU reference within a stated tolerance; at least 10x real time for the P3 domain; memory inside the 2 GB weather budget |
| P5 render link | Solver fields to `CloudFieldProduct` and the volume renderer (density bricks inside L1/L2); the analytic and the simulated sky compared with the cloud split view (`clouds.split_set`) on the same camera and time | No visible seam at the domain edge; the wall cloud and anvil come from the simulation; frame cost inside the cloud budget |
| P6 placement and timewarp (first slice built, see `global-system-status-p6`) | Where on the planet a domain is placed (triggered by the analytic map's convective regions), its lifetime, checkpoints, decay into the analytic map | Scenario 13 (extreme timewarp) passes without the 45 s pre-spin; two launches place the same storm |

## Time and determinism

- A domain is a function of its placement trigger (planet, location, start time, seed) and the analytic map; state at time t is the nearest stored checkpoint at or before t plus a deterministic integration. Checkpoints are written at a fixed cadence and versioned with the solver generation.
- A jump beyond the checkpoint horizon discards the domain; the analytic map covers the sky and a new domain is placed and spun up from its trigger, so the spin-up cost is bounded and cached instead of the current 45 s pre-spin.
- When a domain ends it decays into the analytic map (coverage and precipitation relax over about an hour), so a storm does not vanish when it stops being simulated.

## Hot iteration and parity

Per `docs/ORBIT_HOT_ITERATION.md`: solver state lives in host-owned buffers; kernels are reloadable compute shaders and a versioned module; a failed candidate generation keeps the running one; GPU buffers of an old generation retire only after the GPU completes. Every Studio control has a `weather.*` RPC and MCP tool in the same change (the Weather Lab panel already follows this with `weather_lab.*`).

## Open decisions

1. **Pressure solve across levels.** Multigrid on the nests is the plan; if P2's gate fails, the fallback is a single-level domain at 1 km (about 20 M cells over 500 km, so a smaller domain, about 250 km) with no nests.
2. **Ice.** Warm rain only keeps the cloud top 2 to 3 km low; whether to port Morrison-style ice or a simpler ice/graupel scheme is decided by P1's gate.
3. **Licence.** CM1's distribution terms must be confirmed before copying any CM1 code; running it offline needs no redistribution.

## Diagnose

- A domain produces a different storm on each launch: the seed or a checkpoint is not part of the placement trigger; compare the first `.orbitwx` frames from two launches.
- The cloud field has a seam at the domain edge: the diagnosed `coverage` or `cloudType` is discontinuous across the boundary; use the split view to put the analytic and simulated skies side by side there.
- A nest shows a checkerboard or ringing at the coarse-fine boundary: the interpolated Dirichlet data are not divergence-consistent; check the per-level divergence of rho0 u first.
