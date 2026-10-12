# Sparse storm zones — initial integration slice

Status: experimental, source-level only; runtime integration and execution validation pending.

## Purpose
The planetary weather authority supplies cheap candidate observations. The sparse zone
manager groups nearby candidate locations into persistent computational domains. It
computes one small shared diagnostic profile per zone; no planetary grid of storm
type weights is created. The result is *opportunity*, never mandatory storm spawning.

## API
- `SparseStormZones::Discover(probes, simulatedSeconds)`: call at the provider's
  preferred cadence, not every rendering frame.
- `StormZoneProbe`: direction, screening opportunity, moisture, convection and precipitation.
- `StormZone::weights`: shared lightweight diagnostic state.
- `AnalyticStormProbe`: temporary compatibility bridge for the existing analytic
  `EvaluateWeather`, **not** a physical CAPE/shear/supercell diagnosis.

## Invariants and limitations
- Zones only describe computational regions. Cloud shapes, boundaries, storm types
  and lifetimes are not driven by the zone extent or its scores.
- A high score is not a spawning probability; independent storm entities and
  interaction outcomes are not implemented in this slice.
- Discovery is sparse and capped, but the present candidate clustering is O(N^2).
  Before raising the probe budget, replace pairwise spatial search with a spherical
  spatial hash or local candidate adjacency from the global SIMD solver.
- When a candidate set exceeds maxProbes, the provider must prioritize and send
  only its strongest candidates; the manager processes the first maxProbes.
- `Discover` is state-mutating; run it on a simulation worker and publish an
  immutable snapshot before sharing with rendering or other workers.
- A non-monotonic timestamp resets zone history. Future multi-rate operation needs
  checkpointed event history and explicit temporal versioning.
- Matching uses center proximity. Splits, mergers, shared cold pools, and
  reparenting storm entities require separate identity/interaction logic.
- The current diagnostic weights are temporary proxies: actual physics must use
  source atmospheric profiles (CAPE, CIN, shear, lifting and so forth).
- Existing `StormSchedule` continues to control automatic simulated storm
  placement. This module is not yet wired to Studio or GPU storms.

## Verification
Build and run `ctest -R Orbit.SparseStormZones --output-on-failure`.
Tests cover two regions, clustering, identity continuity, threshold hysteresis,
retirement, backward time and bounded analytic probes.

## Next slices
1. Connect sparse probe emission to the current analytic provider for a visible
   debug zone overlay, while keeping scheduled storms unchanged.
2. Add an atmospheric provider interface for the future global SIMD solver and
   evaluate physical diagnostics once per zone, with local sparse corrections.
3. Introduce procedural convective entities and cross-zone interaction graph.
4. Add timestamped immutable snapshots and a multirate scheduler.
5. Replace fixed tile occurrence authority only after emergent systems are validated.
