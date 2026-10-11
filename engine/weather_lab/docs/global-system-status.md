+++
path = "/world/weather-lab/global-system-status"
title = "Storm domain: measured status of phases 1 to 3"
kind = "concept"
status = "experimental"
summary = """
Measured results of the storm domain phases so far: the CM1 reproduction and the in-engine ice scheme (P1), the nested fast core against a uniform 500 m run (P2), and the 512 km domain with lateral sponge, storm-following frame, adaptive child and cell budget over 6 simulated hours (P3)."""
keywords = ["nesting", "relaxation band", "ice", "cm1", "gate", "results", "storm domain", "child grid"]
sources = [
  "engine/weather_lab/include/orbit/weather_lab/NestedStormSolver.hpp",
  "engine/weather_lab/src/NestedStormSolver.cpp",
  "engine/weather_lab/include/orbit/weather_lab/FastStormSolver.hpp",
]
symbols = ["NestedStormSolver", "NestConfig", "FastStormState"]
invariants = [
  "A child grid is a second FastStormSolver in the parent's Galilean frame with the same vertical grid; its lateral boundary is a relaxation band toward the parent applied before the pressure projection, so the exact FFT solve and round-off divergence per level are kept.",
  "Results here are single runs of one case (supercell, 2 km parent, 120 minutes); storm metrics swing +-20% between cycles, so judge trends, not single rows.",
]
related = ["/world/weather-lab/global-system-plan", "/world/weather-lab/global-system-phases", "/world/weather-lab/sc01"]
depends_on = ["/world/weather-lab/global-system-plan"]
verify = ["orbit_weather_lab nest --minutes 120 --bubble 1 --dt 24 --courant 2.5 --ice 1 --ratio 4 --cells 32 --out child.orbitwx, then compare against a uniform run: orbit_weather_lab run --nx 256 --ny 256 --dx 500 --ice 1 ...", "ctest -R Orbit.WeatherLab"]
verified = "517dbf9"
+++

## Phase status

**P1 (2026-10-11): mostly met.** CM1 reproduced here in Docker (72.5 s for the 2 km, 120 minute case); the in-engine ice scheme brings cloud top to 15-17 km (CM1 15-16) and the updraft to 45-62 m/s (CM1 51-54); the cold pool is 8.5 K against CM1's 7.3 K, about 1.2 K over the 6 to 8 K gate. Details: `sc01`.

**P2 (2026-10-12): accuracy met, cell count scales with the domain.** `NestedStormSolver` (`engine/weather_lab`, `orbit_weather_lab nest`): a child fast core at parent dx / ratio over `cells` parent cells, same frame and vertical grid, relaxed toward the parent in a band of `relaxCells` child cells before each pressure projection (one-way), optionally nudging the parent toward the child's block averages (two-way). It keeps the periodic grid and the exact FFT projection: **no multigrid was needed**, so the multigrid fallback is not built. Per-level divergence stays at 1e-8 /s.

Test case: 128 km parent at 2 km (64x64x40), ice on, 120 minutes, against a uniform 500 m run of the whole domain (256x256x40, 2.6 M cells, 228 s wall).

| Run | Cells | Wall | Peak updraft 50-120 min | UH 2-5 km | Cold pool 120 min | Cloud top |
| --- | --- | --- | --- | --- | --- | --- |
| Uniform 500 m reference | 2.62 M | 228 s | 49-63 m/s | 575-995 | 7.7 K | 15-17 km |
| Nest, 4:1, 64 km child, one-way | 0.82 M | 99 s | 53-64 m/s | 495-917 | 8.3 K | 15-16 km |
| Nest, 4:1, 64 km child, two-way | 0.82 M | 93 s | 46-64 m/s | 578-888 | 7.8 K | 15-16 km |
| Nest, 4:1, 32 km child, one-way | 0.33 M | 36 s | 40-54 m/s | 451-946 | 7.5 K | 14-15 km |

- The 64 km child tracks the reference inside the +-20% cycle-to-cycle spread on updraft, helicity and cold pool, in both modes; two-way is not measurably better than one-way here.
- A 32 km child (7.9x fewer cells) is too small: the storm no longer fits the free interior, updraft runs 15 to 25% low and cloud top 1 to 2 km low. A child needs to be about 2.5 to 3 storm diameters wide.
- The gate's "at least 8x fewer cells" depends on the domain, not the method: at 128 km a 64 km child can only save 3.2x; at the plan's 500 km domain the same 64 km child plus a 2 km base is 3.2 M cells against 40 M for a uniform 500 m grid, 12.5x fewer (arithmetic, not a run).
- Not done: vertical stretching, the 222 m L2 level, moving the child with the storm, open parent boundaries (all phase 3).

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

