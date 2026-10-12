# Sparse storm zones — initial integration slice

Status: experimental. Manual asynchronous diagnostic in Weather Lab is wired; automatic
world-clock discovery, simulation coupling, and execution validation remain pending.

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
- Discovery is sparse and capped. Candidate clustering now uses a Cartesian
  unit-sphere spatial hash with near-neighbor buckets (expected near-linear
  for typical sparse inputs; degenerate dense buckets may still cost O(N^2)).
  A future global SIMD solver can instead emit connected candidate components.
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
  placement. The separate **Weather Lab > Sparse weather zones** inspector
  triggers a read-only global analytic survey via std::async. It does not
  schedule or render storms, and its scan time is an explicit simulated-second
  input rather than the current planetary clock.

## Verification
Build and run `ctest -R Orbit.SparseStormZones --output-on-failure`.
Tests cover two regions, clustering, identity continuity, threshold hysteresis,
retirement, backward time and bounded analytic probes.

## Diagnostic integration
- `AnalyticZoneSurvey.hpp` implements camera-independent, equal-area sampling;
  only top-ranked candidate probes survive its hard count budget.
- `WeatherLabUi` launches the scan on a worker via `std::async`, polls completion
  without blocking its draw loop, and prints the first 12 zones with diagnostic
  proxy values. The UI is deliberately read-only.
- The survey uses default analytic weather parameters and default climate;
  it is not yet attached to the active planet's climate or clock.

## Next slices
1. Bind the survey to the active body's weather parameters, climate and
   simulation time, then expose an actual planetary map overlay.
2. Add an atmospheric provider interface for the future global SIMD solver and
   evaluate physical diagnostics once per zone, with local sparse corrections.
3. Add timestamped snapshots and budgeted multirate scheduling, followed by
   procedural convective entities and the interaction graph.
4. Replace fixed tile occurrence authority only after emergent systems are validated.
