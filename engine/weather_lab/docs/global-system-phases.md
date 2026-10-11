+++
path = "/world/weather-lab/global-system-phases"
title = "Global weather system: timewarp, hot iteration, phases and open decisions"
kind = "concept"
status = "experimental"
summary = """
Second half of the global weather plan: how time and checkpoints keep weather deterministic under timewarp, how the solvers stay hot-reloadable, the six phases with measurable gates, and the decisions still open."""
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

## Timewarp and determinism

- State at time t = the nearest stored checkpoint at or before t plus a deterministic integration. Checkpoints are written at a fixed cadence (daily for T1) and are versioned with the solver generation.
- A jump larger than the checkpoint horizon goes to climate-only state and re-spins from there; this replaces the current 45 s pre-spin with a bounded, cached cost.
- Nests are functions of the parent state and a trigger seed, so two launches spawn the same storm at the same time.

## Hot iteration and parity

Per `docs/ORBIT_HOT_ITERATION.md`: solver state lives in host-owned buffers; kernels are reloadable compute shaders and a versioned module; a failed candidate generation keeps the running one; GPU buffers of an old generation retire only after the GPU completes. Every Studio control has a `weather.*` RPC and MCP tool in the same change (the Weather Lab panel already follows this with `weather_lab.*`).

## Phases and gates

| Phase | Work | Gate |
| --- | --- | --- |
| P1 reference | CM1 buildable here (Docker or a WSL distribution), SC-01 re-run on this machine; ice and graupel plus latent heat of freezing in the fast core | Cold pool 6 to 8 K, updraft helicity within CM1's band, cloud top 15 to 16 km at 2 km |
| P2 GPU port | Vulkan compute port of the fast core as a hot module | Matches the CPU reference within a stated tolerance; at least 10x real time at 64x64x40; memory inside the 2 GB weather budget |
| P3 boundaries and nesting | Open lateral boundaries, moving frame, one-way nesting from a prescribed parent | Outflow does not re-enter; a nest reproduces a larger-domain run's storm |
| P4 render link | Solver fields to `CloudField` and the volume renderer; the analytic and the simulated sky compared with the cloud split view (`clouds.split_set`) on the same camera and time | No visible discontinuity at tier hand-off; frame cost inside the cloud budget |
| P5 global tier | Cubed-sphere T1 core seeded from `EvaluateWeather`, climate forcing | Stable 10-year run; storm-track position and ITCZ seasonal motion match the analytic map within a stated tolerance |
| P6 scheduler and timewarp | Triggers, nest lifetime, checkpoints, decay of ended nests | Scenario 13 (extreme timewarp) passes without the 45 s pre-spin |

## Open decisions

1. **CM1 in the runtime or not.** This plan says oracle and bake source only. A different answer changes P1 and P2 substantially.
2. **T1 core.** A cubed-sphere primitive-equation core written here, against adapting an existing open global model; the plan assumes written here because the repository is C++/Vulkan only.
3. **Licence.** The distribution terms of CM1 and of any adapted global model must be confirmed before copying code.

## Diagnose

- A nest produces a different storm on each launch: a seed or checkpoint is not part of the nest's initial state; compare the nest's `.orbitwx` first frames from two launches.
- The cloud field has a seam at a nest boundary: the diagnosed `coverage` or `cloudType` is discontinuous across the hand-off; use the split view to put the analytic and simulated skies side by side at that location.
