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

## Phase 3: live clock-driven runtime (experimental)
- `StormZoneRuntime.hpp` owns an asynchronous, cadence-gated opportunity survey,
  zone discovery, and immutable reference-counted snapshots.
- `StudioViewportRenderer::UpdateStormDomain` now forwards the actual simulation
  timestamp and active-body generation, plus the active cloud layer's weather
  parameters. The current provider has no complete active climate sample, so
  the climate argument remains the documented analytic default.
- `storm.status` and Weather Lab show published zone count, candidate count,
  generation, snapshot time and worker activity. Survey data does NOT change
  automatic storm selection or cloud rendering.
- Tests: `ctest -R "Orbit.SparseStormZones|Orbit.StormZoneRuntime"`.
- The scheduler currently has a single survey cadence and does not yet manage
  each individual zone's own simulation interval or physical interaction.
- A snapshot may lag the displayed planetary time while asynchronous work runs.
  During jumps, the newer survey supersedes older published state; do not use
  snapshots as authoritative weather physics until timestamped coupling exists.
- Build, CI and device performance need independent verification.

## Phase 4: sparse multirate scheduler (planning only)
- `WeatherMultirateScheduler.hpp` maintains distinct simulated-time cursor,
  update cadence and budget accounting for each persistent zone identity.
- `Reconcile` adds/removes zones when a new immutable discovery snapshot
  arrives; `Plan` creates at most one bounded ticket per due zone, irrespective
  of the magnitude of the requested time jump.
- For a time jump exceeding `maximumDetailedStep`, the ticket kind is
  `RequiresLongStepModel`. This is a **blocked obligation** until a validated
  long-step model and exchange ledger exist. NO physical state is advanced
  by issuing a ticket, and the viewport deliberately does not call `Commit`.
- The runtime currently has no physical zone evolution. `storm.status` and
  Weather Lab expose ticket count, blocked long steps, deferred task count
  and maximum simulated-time lag. The old storm scheduler and renderer run
  as before; this scheduler only observes and plans.
- The computational budget is measured in estimated scheduling units rather
  than benchmarked milliseconds. Do not equate the values with measured
  CPU or GPU cost.
- The current scheduler does not dispatch worker tasks, build child-structure
  dependency graphs, reconcile exchange budgets or preserve historical storm
  identities through arbitrary rewind. Those require further implementations.
- Validation command:
  `ctest -R "Orbit.WeatherMultirateScheduler|Orbit.StormZoneRuntime|Orbit.SparseStormZones" --output-on-failure`.
- Recommended next: add an explicit `IWeatherEvolutionOperator` completion
  protocol with timestamped flux ledgers, and only then enable ticket execution
  for suitable model types. Coarse temporal skipping must never masquerade
  as successful physical integration.

## Phase 5: asynchronous, transactional short-step surrogate

- `WeatherEvolution.hpp` contains `WeatherConvectiveSurrogate`,
  `WeatherEvolutionLedger` and `WeatherZoneEvolutionRunner`.
- Each discovered zone initializes three deterministic convective child states,
  each with its own normalized moisture inventory. These are intentionally
  experimental perturbations, NOT physically calibrated storm cells.
- The short-step operator partitions conserved normalized inventory among
  vapor, condensate, precipitation and exported rain. The integrated export is
  recorded in a time-stamped exchange ledger.
- `WeatherEvolutionLedger::Complete` checks a matching ticket, coherent
  timestamps and exchange metadata before committing both state and scheduler
  progress. Unfinished tickets never advance simulated time.
- One shared `std::async` worker runs only the selected eligible short-step
  ticket; no per-zone threads. Results are applied on the runtime-owner thread.
- Long-step timewarp tickets are **not executed**. They still require an
  independently validated coarse evolution operator with integrated feedback.
- Exchange records are retained, with a hard 4096-record capacity; execution
  stops when capacity is exhausted rather than silently dropping exchange.
  A real global solver needs a drain/acknowledgment consumer before production.
- Rewinding or switching planets resets the *prototype's* zone state and ledger.
  This cannot be used as an authoritative persistent weather history.
- Rendering and the existing GPU storm scheduler remain unchanged. The
  surrogate's normalized moisture and updraft values MUST NOT be rendered or
  interpreted as meteorological predictions.
- Live diagnostics expose completed steps and outstanding exchange records
  in Weather Lab and `storm.status`.
- Tests: `ctest -R "Orbit.WeatherEvolution|Orbit.WeatherMultirateScheduler|Orbit.StormZoneRuntime" --output-on-failure`.
- Still required: a physical atmospheric input profile, conservative exchange
  acknowledgment by a global solver, interaction topology, pressure/energy
  coupling, calibrated temporal errors, a validated long-step operator,
  and benchmarks on production hardware.

These changes are committed but have not yet been compiled or run on the
development machine.

## Phase 6: convective interaction graph (experimental)

- `WeatherConvectiveInteractions.hpp` adds a sparse, undirected graph over
  the convective children inside a discovered zone. Node identity is stable;
  connectivity is based on local offsets and a bounded distance kernel.
- All pair edges are canonical (ascending IDs); connected components over
  sufficiently strong edges become *coupling islands*. An island means shared
  numerical scheduling is possible, **not** that the cells have merged.
- On validated short detailed updates, `EvaluateInteractingWeather` first runs
  the Phase 5 surrogate, then applies simultaneous pairwise interactions from
  an immutable state copy. Vapor transfers are symmetrical and limited by
  available inventory across multiple neighbors; diagnostic updraft intensity
  may increase or decrease through simplified lift/competition response.
- Positions are local-zone kilometre offsets; interactions are synthetic
  placeholders, not CM1-calibrated cold-pool dynamics or physical wind profiles.
  No actual storm deformation, collision, merger, splitting, advection or
  cross-zone coupling is simulated in this slice.
- A bounded child/edge budget avoids unbounded allocations. The current
  per-zone pair construction is O(children^2) with a hard maximum of 64;
  replace with a spatial hash when larger systems are required.
- The live shared worker uses this operator via `SetEvaluator`; transactional
  inventory checks and precipitation export accounting remain unchanged.
- Long-step obligations remain blocked and the actual global atmosphere still
  does not consume conserved exchanges. No changes to GPU storm rendering.
- Validate with:
  `ctest -R "Orbit.WeatherConvectiveInteractions|Orbit.WeatherEvolution|Orbit.WeatherMultirateScheduler" --output-on-failure`.
- Validation and compilation have not yet been run. Before calling Phase 6
  production-ready, add true physical forcing, event-aware coupling/merger,
  cross-zone neighbors, deterministic checkpointing and hardware benchmarks.

## Phase 7: emergent organization and cold-pool influence (experimental)

- `WeatherEmergence.hpp` extends the Phase 6 conservative surrogate with
  bounded local drift, an evolving cold-pool footprint and normalized strength,
  edge lifting, and interior undercutting. This is a low-cost structural
  demonstrator, NOT a resolved density-current model.
- Existing convective child positions are advanced from a frozen pre-step
  snapshot to avoid sequential/order-dependent updates. No new vapor,
  condensate or precipitation is generated by these geometric interactions.
- The same sparse interaction graph identifies peer components. Components
  record **duration of coherent connectivity** and receive persistent IDs
  only for unchanged membership. Disconnection automatically dissolves
  the group; transient contacts do not become storm-system parents.
- A group is explicitly **not** categorized as a supercell, multicell,
  squall line or other storm type. Favorable zone weights alone cannot
  create an organized system.
- A few original seed convective elements remain: birth, decay/removal,
  physically justified splitting, merging, cold-pool momentum exchange,
  cross-zone neighbors and stable reparenting are not built yet.
- `StudioViewportStorm.cpp` selects `EvaluateEmergentWeather` for the
  existing shared asynchronous zone runner. It does not touch the GPU
  storm solver, cloud renderer, CM1 baked storm or old storm placement.
- Tests: `ctest -R "Orbit.WeatherEmergence|Orbit.WeatherConvectiveInteractions|Orbit.WeatherEvolution" --output-on-failure`.
- All quantities and coefficients remain uncalibrated placeholders. This
  code has not been compiled, benchmarked or runtime-tested in this session.

The recommended next work is to verify the branch builds before adding
complexity, then implement a compact spatial perturbation registry shared
between neighboring zones with physically grounded inflow, cold-pool and
vorticity data from the future global atmospheric provider.

## Phase 8: sparse cross-zone perturbations (experimental)

- `WeatherCrossZoneField.hpp` stores compact, timestamped cold-pool
  diagnostic sources in body-fixed unit-sphere coordinates. Local cell
  east/north offsets are mapped to the planetary sphere using a tangent frame.
  Unlike the zone tree, the disturbance registry crosses zone boundaries.
- The active `WeatherZoneEvolutionRunner` rebuilds its source registry only
  on completion of a detailed zone update or zone discovery changes. Each
  source carries its originating zone and child IDs, source time, radius and
  normalized strength. Sources are bounded (1024 records by default).
- A receiving zone samples the **last committed source state** at its own
  step start. Records from the future, too far away, older than the configured
  900 simulated-second horizon, or from the receiver's own zone are ignored.
  Sampled forcing is a frozen input to the background worker, never an
  unsafe mutable reference across concurrent simulations.
- Outflow edge lift and internal undercutting influence the *experimental
  normalized updraft diagnostic* at the next short-step evolution.
  No cloud shape, real momentum, moisture or energy coupling is claimed.
- Cross-zone state is discarded on runtime reset (rewind or body switch).
  Source provenance and age ensure an outdated sender cannot directly
  overwrite a newer receiver state.
- Storm zones remain organizational opportunities, not physical barriers.
  Cross-zone sources do **not** merge zones or force supercell classification.
- Tests: `ctest -R "Orbit.WeatherCrossZoneField|Orbit.WeatherEmergence|Orbit.WeatherEvolution" --output-on-failure`.
- Follow-on work: physical global-atmosphere flux exchange with explicit
  acknowledgments, a global spatial hash for source queries when 1024 sources
  is insufficient, cross-zone synchronization islands for tightly coupled
  events, persistent event history, and independent build/run validation.
- The source records currently represent stationary, decaying influence
  footprints. Advective transport of the actual cold-pool front and true
  boundary-layer dynamics are not implemented.
