# Two-layer drainage halo and serialized adjacent page builds

- Existing owners: M09 `DrainagePage`/`DrainagePageHalo` (flow, conditioning,
  injection), `StudioTerrainPhysicalPageService::BuildDrainageHalo` (halo
  assembly) and `StudioTerrainRebuildScheduler` (build submission).
- Problem: pages share their edge column, but the halo held the neighbour's
  copy of that same column. Each page lifted its copy above the twin and routed
  into it, and adjacent pages rebuilt from the same stale snapshot swapped
  states forever (the Studio round-trip verifier failed ~60% of runs and the
  terrain validation scenario took ~30 s of constant rebuilds).
- Change: halo carries the neighbour's cell beyond the shared edge plus the
  neighbour's twin copy (conditioned-height floor and crossing-flow
  injection); upstream outward cells are not drains; `CellAsBoundary` exports any
  cell; the scheduler gains `SetPageGate` and the page service uses it to build
  edge-adjacent pages one after another.
- Canonical state: none added. Everything is derived M09 state.
- Tests: `TestSeamBasinExchangeConverges`, `TestGridSeamExchangeConverges`
  (2x2 grid, 60 seeds, must converge and conserve 324 m2) in
  `DrainagePageTests.cpp`; `TestPageGateSerializesDependentPages` in
  `StudioTerrainRebuildSchedulerTests.cpp`.
- Known limit: simultaneous (stale-snapshot) rebuilds of adjacent pages still
  fail to converge for ~7% of seeds, which is why the gate exists. Diagonal
  pages are not consulted (corner ring cells are walls).
- Iteration: native changes use the central Studio generation handoff.
