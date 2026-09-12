# Long-Range Sparse Hydrology / Planet Studio Water Sources

<!-- ASTERRA_PLAN_ID: WATER_LONG_RANGE_HYDROLOGY -->
<!-- ASTERRA_WATER_LONG_RANGE_STATUS: NOT_STARTED -->
<!-- ASTERRA_WATER_LONG_RANGE_TASK_COMPLETE: false -->

**Target branch:** `pre/0.1.0` only  
**Implementation model:** extend the existing production sparse/regional hydrology stack; do not create a parallel water simulator.  
**Primary invariant:** **simulation residency follows water, not the camera.**

## Final completion gates

These markers are authoritative. Do not mark them complete early.

- [ ] IMPLEMENTATION COMPLETE
- [ ] AUTOMATED VALIDATION COMPLETE
- [ ] MANUAL PLANET STUDIO ACCEPTANCE COMPLETE
- [ ] TASK COMPLETE

The task is complete only when all four boxes above are `[x]` and the marker at the top reads:

```text
<!-- ASTERRA_WATER_LONG_RANGE_STATUS: COMPLETE -->
<!-- ASTERRA_WATER_LONG_RANGE_TASK_COMPLETE: true -->
```

---

## 0. Required behavior

The finished system must support a Cities: Skylines-like continuous source on the authored Asterra planet:

1. Place a source on terrain in Planet Studio.
2. Give it a continuous volumetric flow rate in m³/s.
3. Water is inserted into the authoritative hydrology simulation continuously.
4. Water follows terrain/gravity through the existing shallow-water / finite-volume solver.
5. It can cross many sparse tile boundaries and travel tens or hundreds of kilometres.
6. Depressions fill into lakes instead of deleting or freezing the incoming volume.
7. When a basin reaches its sill/outlet, overflow naturally continues downstream.
8. Moving the camera away does not stop, reset, unload, or destroy active hydrological state.
9. Returning to a distant river/lake reconstructs the current authoritative state for rendering.
10. If a flow reaches the ocean, the coast behaves as a controlled outflow boundary and does not trigger unbounded sparse-ocean tile allocation.
11. Source definitions survive the normal Planet Studio save/reload workflow.
12. Resource budgets may reduce resolution or activity frequency, but may never silently delete non-zero water mass.

### Non-goals

- Do not replace physical flow with scripted river splines or pathfinding.
- Do not simulate the entire planet at maximum hydrology resolution.
- Do not make the render cache authoritative for water mass.
- Do not tie simulation lifetime to the active camera/viewer radius.
- Do not rewrite the ocean system as part of this task.
- Do not redesign unrelated Planet Studio UI.
- Do not create a new branch or PR for this task.

---

## 1. Repository execution contract

<!-- PHASE_REPO_CONTRACT: NOT_STARTED -->

Before editing anything, run from the repository root:

```bash
git branch --show-current
git status --short
git rev-parse HEAD
git log -5 --oneline
```

Required result:

- branch is exactly `pre/0.1.0`;
- unrelated local changes are preserved;
- the current HEAD is recorded in the validation log at the bottom of this file.

If the branch is not `pre/0.1.0`, stop and switch to the existing `pre/0.1.0`. **Do not create a branch.**

Before every implementation commit, repeat:

```bash
git branch --show-current
git status --short
git diff --check
```

Do not use force-push or destructive reset to deal with concurrent changes.

### Acceptance gate

- [ ] Current base SHA recorded.
- [ ] Confirmed current branch is `pre/0.1.0`.
- [ ] Existing working-tree changes, if any, identified and preserved.

When all three pass, change:

```text
<!-- PHASE_REPO_CONTRACT: COMPLETE -->
```

---

## 2. Phase 0 — Audit the current production stack

<!-- PHASE_0_BASELINE_AUDIT: NOT_STARTED -->

Do this before implementation because `pre/0.1.0` is actively changing. In particular, inspect the current regional hydrology/reanalysis work rather than assuming the older sparse runtime is still the only authority.

### 2.1 Inspect the current HEAD and hydrology topology

Run:

```bash
git show --stat --oneline HEAD
git log --oneline -- scripts/water scripts/world_authoring | head -40
find scripts/water -maxdepth 2 -type f | sort
find scripts/world_authoring -maxdepth 3 -type f | sort | grep -E 'water|hydro|planet_studio'
grep -n "WaterSystem" project.godot
grep -RInE "regional|reanalysis|sparse_hydro|hydrology|point_water_source|upsert_point_water_source" scripts/water scripts/world_authoring | head -300
```

Read the current versions of, at minimum, if they still exist:

- `project.godot`
- `scripts/water/water_system.gd`
- every active `scripts/water/water_system*.gd` implementation referenced by the autoload
- `scripts/water/sparse_hydrology_runtime.gd`
- `scripts/water/sparse_hydro_scheduler.gd`
- `scripts/water/hydro_source_ingress.gd`
- `scripts/water/hydro_source_terms_gpu.gd`
- all new regional hydrology/reanalysis files reachable from the current production path
- `scripts/water/sparse_hydro_surface_cache_bridge.gd`
- `scripts/water/local_water_surface.gd`
- `scripts/world_authoring/planet_studio_water_source_bridge.gd`
- `scripts/world_authoring/model/water_point_source_definition.gd`
- `scripts/world_authoring/model/water_authoring_profile.gd`

### 2.2 Produce an authority/lifetime map in this file

Under **Audit notes** below, record the exact current owners of:

- water source definitions;
- source-to-tile mapping;
- source injection;
- active sparse/regional tile allocation;
- tile retirement;
- wet/dry detection;
- face/halo exchange;
- LOD/refinement/coarsening;
- persistent/dormant hydrology state;
- ocean/coast boundary handling;
- visible surface reconstruction;
- camera/viewer candidate generation;
- simulation timestep/subcycling and CFL limits;
- hard/soft tile budgets.

Explicitly identify every code path where viewer/camera distance can cause a simulation tile or its state to be retired.

Explicitly identify every code path where non-zero water state can be discarded, overwritten, reinitialized, or replaced during allocation, retirement, LOD transition, save/load, or reanalysis.

### Acceptance gate

- [ ] Exact production `WaterSystem` implementation identified.
- [ ] Regional reanalysis layer and sparse runtime relationship documented.
- [ ] All camera-driven residency paths identified.
- [ ] All non-zero-state destruction/reinitialization paths identified.
- [ ] Current source ingress lifecycle traced end-to-end.
- [ ] Current boundary/halo exchange traced across a tile edge.
- [ ] Existing scheduler frontier allocation and dry hysteresis behavior documented rather than duplicated.

Do not begin Phase 1 until these are true.

---

## 3. Phase 1 — Add hydrology accounting and observability first

<!-- PHASE_1_ACCOUNTING: NOT_STARTED -->

Before changing residency rules, make water conservation measurable.

### Required counters/state

Add authoritative runtime accounting for, at minimum:

- cumulative source volume injected, m³;
- current stored inland water volume, m³;
- cumulative ocean/coastal discharge, m³;
- cumulative intentional boundary discharge other than ocean, if any;
- numerical/residual mass error, m³ and percentage;
- active simulation tile count;
- wet tile count;
- pinned source tile count;
- pending/active frontier count;
- dormant/coarsened wet tile count if such a state exists;
- frontier allocations refused/delayed because of budget;
- wet-state retirement attempts prevented by conservation rules.

Use the actual cell areas/metrics of the planetary grid. Do not estimate global volume from a flat constant area if the current solver already has metric-aware areas.

The accounting identity to expose is conceptually:

```text
stored(t) - stored(t0)
  ~= injected
   - ocean_discharge
   - other_intentional_discharge
   + numerical_residual
```

Expose a compact read-only stats snapshot through the existing water-system API so tests and Planet Studio diagnostics do not reach into scheduler internals.

### Likely files

Use current audit results rather than blindly editing these, but likely touch:

- `scripts/water/sparse_hydrology_runtime.gd`
- current regional hydrology runtime/reanalysis files
- `scripts/water/hydro_source_terms_gpu.gd`
- current `scripts/water/water_system*.gd`

### Acceptance gate

- [ ] Injected volume is measurable.
- [ ] Stored inland volume is measurable.
- [ ] Ocean discharge is measurable or explicitly zero before coast work.
- [ ] Tile residency/frontier counters are measurable.
- [ ] A test/debug caller can read a coherent snapshot through `WaterSystem`.
- [ ] No rendering component is used as the accounting authority.

---

## 4. Phase 2 — Pin enabled water-source tiles

<!-- PHASE_2_SOURCE_PINNING: NOT_STARTED -->

An enabled point source must keep its simulation state alive even with no viewer nearby.

### Required implementation

1. Resolve each enabled source's canonical planet-fixed direction to the authoritative simulation tile/LOD used by source ingress.
2. Register a source pin against that tile.
3. Use set/refcount semantics so multiple sources can share a tile safely.
4. A pinned source tile cannot be retired because it is outside the camera/viewer working set or because it was briefly dry between source updates.
5. On source move or tile/LOD remap:
   - acquire the new pin first;
   - synchronize the source mapping;
   - then release the old pin.
6. On disable/delete:
   - stop future injection;
   - remove the source mapping;
   - release its pin;
   - normal wet/dry residency rules decide when the tile can later retire.
7. Source synchronization must remain idempotent across Planet Studio polling/resync.

### Likely files

- `scripts/water/hydro_source_ingress.gd`
- `scripts/water/sparse_hydrology_runtime.gd`
- `scripts/water/sparse_hydro_scheduler.gd`
- regional scheduler/runtime equivalents found in Phase 0

### Acceptance gate

- [ ] Source continues accumulating/injecting while camera is far away.
- [ ] Multiple sources on one tile do not prematurely unpin each other.
- [ ] Moving a source does not create a frame where neither old nor new state is valid.
- [ ] Disable/delete cleanly releases only that source's pin.

---

## 5. Phase 3 — Make wet-state residency water-driven

<!-- PHASE_3_WET_RESIDENCY: NOT_STARTED -->

Strengthen the existing scheduler; do **not** throw away its current frontier allocation or dry hysteresis.

### Per-tile metadata

Use equivalent existing fields when available. Add only what is missing:

- source pin/reference set or count;
- current total volume;
- maximum/representative depth for wet-state decisions;
- last wet simulation tick/time;
- unresolved outward-flux/frontier mask;
- dormant/coarsened-state reference if supported;
- reason flags useful for diagnostics (`SOURCE`, `WET`, `FRONTIER`, `VIEWER`, etc.).

### Residency rule

A simulation tile must remain authoritative while **any** of these are true:

- it contains an enabled source pin;
- conserved water volume is above the validated wet epsilon;
- required wet-depth criteria are above epsilon;
- it was wet inside the dry-hysteresis interval;
- it has unresolved outward flux/frontier work;
- a conservative LOD/coarsening transfer is still pending.

Viewer proximity may increase simulation/refinement priority and may add dry candidate tiles, but it must not be a requirement for the continued existence of wet state.

### Retirement rule

A tile may be fully destroyed only when:

1. it is unpinned;
2. its conserved water volume is below the validated dry threshold;
3. it has no unresolved face/frontier transfer;
4. dry hysteresis has expired;
5. no pending conservative parent/child transition references it.

Never fix a budget overrun by deleting a wet tile's mass.

### Acceptance gate

- [ ] Distant wet tile remains authoritative after camera leaves.
- [ ] Distant lake continues receiving inflow.
- [ ] Returning camera reconstructs current state, not the state from departure time.
- [ ] Truly dry, unpinned tiles still retire after hysteresis.

---

## 6. Phase 4 — Propagate the wet frontier across sparse tile boundaries

<!-- PHASE_4_FRONTIER: NOT_STARTED -->

This is the key to arbitrarily long rivers without preallocating the planet.

### Required behavior

1. Detect an approaching wet front before a missing neighbour becomes a hard numerical boundary.
2. Use existing face fluxes and/or an edge-depth guard band to determine which neighbour is required.
3. Queue neighbour allocation deterministically.
4. Initialize neighbour terrain/metric/boundary data before its first hydrology update.
5. Establish the correct ghost/halo/face exchange before allowing meaningful outflow into the neighbour.
6. Transfer mass and momentum conservatively according to the existing solver convention.
7. Preserve face continuity across equal-LOD and supported mixed-LOD boundaries.
8. Do not repeatedly allocate/deallocate a frontier neighbour each frame; reuse existing hysteresis/state.
9. If a budget delays the neighbour, retain the upstream state/flux safely and expose the stall through diagnostics rather than losing volume.

### Prefetch rule

If the solver requires a neighbour to exist before computing an outward flux, allocate from a near-edge wet/depth/velocity condition rather than waiting for a flux that can never exist without the neighbour.

The exact guard-band width and epsilon must come from grid spacing, timestep, and solver behavior—not a camera radius.

### Acceptance gate

- [ ] Controlled sloped-channel test crosses many tile boundaries.
- [ ] No visible/diagnostic mass discontinuity at a boundary.
- [ ] Frontier advances with camera stationary elsewhere.
- [ ] Tile creation does not fan out uncontrollably into dry neighbours.

---

## 7. Phase 5 — Preserve distant wet state under LOD/budget pressure

<!-- PHASE_5_PERSISTENCE_COARSENING: NOT_STARTED -->

A long river cannot keep every historical cell at maximum resolution forever. Prefer the existing regional/HLOD mechanism introduced on `pre/0.1.0` if it can represent conserved hydrology state safely.

### Required policy

Priority order when reducing cost:

1. reduce update frequency where numerically acceptable;
2. migrate/refine/coarsen through existing regional hierarchy conservatively;
3. keep a compact dormant wet representation that preserves conserved quantities;
4. only fully retire when the tile is genuinely dry under Phase 3 rules.

For a conservative coarse transfer, preserve at minimum the quantities the production solver requires to resume without inventing/removing water. Typically this means integrated water volume/depth and the appropriate momentum representation. Follow the existing numerical formulation rather than inventing incompatible averaging.

When a coarse/dormant region becomes active again because of:

- an approaching wet frontier;
- a nearby viewer requesting visible reconstruction;
- a local source;
- a refinement/reanalysis event;

restore/refine it deterministically and conservatively.

### Safety fallback

If the current regional system cannot safely coarsen wet SWE state yet, **keep wet sparse tiles resident** and mark conservative coarsening as a blocker. Do not implement lossy eviction as a temporary shortcut.

### Acceptance gate

- [ ] Water survives beyond all viewer-local radii.
- [ ] LOD/coarsening transition preserves volume within the numerical tolerance defined by tests.
- [ ] Reactivation does not reset depth/momentum to a dry initial condition.
- [ ] No wet mass is discarded to satisfy a tile cap.

---

## 8. Phase 6 — Implement coast/ocean discharge as a real boundary

<!-- PHASE_6_OCEAN_BOUNDARY: NOT_STARTED -->

When inland hydrology reaches the ocean, it should leave the sparse inland domain instead of causing an endless chain of ocean tile allocations.

### Required behavior

1. Identify coast/ocean cells/faces from the existing planet/ocean/terrain authority found in Phase 0.
2. Treat qualifying faces as production open/outflow boundaries.
3. Account discharged water volume in the Phase 1 counters.
4. Do not allocate ordinary downstream inland hydrology tiles merely to represent open ocean where the ocean renderer/system is already authoritative.
5. Use the production ocean level/boundary model for allowable backpressure/backflow if the solver supports it; otherwise document and test the chosen one-way outflow approximation.
6. Ensure coast detection is stable across LOD/reanalysis.

### Acceptance gate

- [ ] Test river reaches coast and drains.
- [ ] Ocean discharge counter matches removed inland volume within tolerance.
- [ ] Sparse tile count stabilizes instead of expanding across the ocean.
- [ ] Coastal flow does not disappear before reaching the valid ocean boundary.

---

## 9. Phase 7 — Budgets, scheduling priority, stability, and determinism

<!-- PHASE_7_BUDGET_STABILITY: NOT_STARTED -->

### Scheduling priority

Under pressure, prioritize roughly:

1. source-pinned simulation work;
2. wet frontier / required neighbour work;
3. active wet-state integration;
4. conservative transition/coarsening work;
5. viewer-requested dry/refinement candidates;
6. presentation/cache work.

Adapt this ordering to the current scheduler if dependencies require a different safe sequence.

### Required protections

- Explicit soft/hard counts for active tiles and pending frontier work.
- No hard cap may silently delete conserved wet state.
- A blocked frontier must produce a visible diagnostic/counter.
- CFL/subcycling limits remain authoritative even if debug simulation speed is later increased.
- Allocation and frontier traversal use deterministic ordering for a fixed initial state/input sequence.
- Reanalysis must not overwrite newer live wet state with stale coarse/dry state.

### Acceptance gate

- [ ] Stress case cannot silently lose mass at the budget limit.
- [ ] No runaway allocation on a static lake or after reaching ocean.
- [ ] Same deterministic test produces equivalent topology/state within the solver's expected tolerance.
- [ ] Regional reanalysis and live sparse integration have an explicit conflict/ownership rule.

---

## 10. Phase 8 — Planet Studio test integration

<!-- PHASE_8_PLANET_STUDIO: NOT_STARTED -->

Preserve the existing point-source authoring workflow. Do not rebuild it unless Phase 0 proves it has changed.

Expected existing authoring capabilities to retain:

- add source;
- terrain placement from canonical planet direction;
- name/source ID;
- m³/s rate;
- enable/disable;
- move/re-place;
- delete;
- runtime resync;
- inland/flood render toggle;
- save/reload through the water authoring profile.

### Add read-only hydrology diagnostics

Expose enough of the Phase 1 snapshot in Planet Studio to test long-running sources without a debugger:

- source state/rate;
- active tile count;
- wet tile count;
- pinned tile count;
- frontier count/stalls;
- current stored volume;
- cumulative injected volume;
- cumulative ocean discharge;
- mass residual/error.

The UI may request local visible reconstruction/recentering, but those controls must never become the authority that enables simulation.

### Optional debug acceleration

- [~] Add `x10` / `x100` simulation-speed controls **only** if the existing fixed-step/subcycling/CFL implementation can maintain stable integration. Otherwise defer them rather than compromising the solver.

### Acceptance gate

- [ ] Existing source editing still works.
- [ ] Saved source reloads and rebinds to runtime.
- [ ] Hiding local water rendering does not stop hydrology.
- [ ] Recentring surface cache changes presentation only.
- [ ] Diagnostics reflect the authoritative runtime.

---

## 11. Phase 9 — Automated validation

<!-- PHASE_9_AUTOMATED_VALIDATION: NOT_STARTED -->

First discover and use the repository's actual current test convention. Do not introduce an unrelated testing framework just for this task.

From repo root:

```bash
find tests -maxdepth 3 -type f | sort | head -200
find . -maxdepth 3 -type f \( -name '*test*.gd' -o -name '*test*.py' -o -name '*test*.sh' \) | sort | head -300
grep -RInE "--headless|gut|GdUnit|test runner|run_tests" README.md docs tests scripts 2>/dev/null | head -200
```

Add deterministic tests/fixtures following the discovered convention.

### Mandatory scenarios

#### A. Long sloped multi-tile channel

- Fixed terrain fixture with known downhill path.
- Continuous source.
- Camera/viewer deliberately located elsewhere.
- Front must cross a meaningful number of sparse/regional boundaries.
- Assert no stall caused by lack of viewer residency.

#### B. Basin fill and sill overflow

- Closed depression with a known outlet elevation.
- Assert stored volume rises before outlet activation.
- Assert downstream flow starts only after surface reaches the sill within numerical tolerance.
- Continue long enough to verify downstream propagation.

#### C. Camera independence

Run the same source/terrain twice:

1. viewer follows the water;
2. viewer stays far away or moves elsewhere.

Compare conserved state/topology at fixed simulation ticks. Results must agree within the production numerical tolerance.

#### D. Source lifecycle

- enable;
- move/rebind;
- disable;
- delete;
- shared-tile multiple sources.

Assert pin counts and injection accounting are correct.

#### E. Ocean discharge

- Deterministic channel terminating at a coastal boundary.
- Assert volume leaves inland domain through accounted ocean discharge.
- Assert no unbounded ocean tile allocation.

#### F. Mass balance

At selected ticks calculate:

```text
mass_error = stored_delta - injected + ocean_discharge + other_discharge
```

Define absolute and relative tolerances from actual solver precision/grid scale. Record those tolerances in the test and validation log. Do not choose a tolerance so loose that a lost tile can pass.

#### G. Persistence / coarsening / reactivation

Force the flow beyond the high-resolution working region or trigger the existing regional transition. Assert conserved state survives and returns correctly when reactivated/refined.

#### H. Authoring persistence

Serialize/reload the point source through the current Planet Studio/world-authoring model and verify source ID, direction, rate, tile-level policy, and enabled state are preserved.

#### I. Budget stress

Use a constrained test budget. Assert the system reports blocked/deferred work without deleting wet mass.

### Validation commands

Record the exact commands actually run in **Validation log**. At minimum run:

```bash
git diff --check
```

Then run the repository's discovered headless/unit/integration test command. If Godot has an environment-specific Vulkan/headless failure, record it separately. An environmental failure is not a passing validation.

### Acceptance gate

- [ ] Long channel passes.
- [ ] Basin fill/overflow passes.
- [ ] Camera independence passes.
- [ ] Source lifecycle passes.
- [ ] Ocean discharge passes.
- [ ] Mass balance passes.
- [ ] Persistence/coarsening passes.
- [ ] Authoring persistence passes.
- [ ] Budget stress passes.
- [ ] `git diff --check` passes.

Only after all mandatory boxes pass may `AUTOMATED VALIDATION COMPLETE` be checked at the top.

---

## 12. Phase 10 — Manual Planet Studio acceptance test

<!-- PHASE_10_MANUAL_ACCEPTANCE: NOT_STARTED -->

Use the actual Asterra planet, not only synthetic fixtures.

### Test procedure

1. Launch Planet Studio on the production Asterra body.
2. Open the Water authoring category.
3. Add a point source on elevated terrain.
4. Set a clearly observable test rate, initially **100 m³/s** unless local terrain/grid scale makes that numerically unreasonable.
5. Enable the source.
6. Enable inland/flood rendering.
7. Confirm injection/stored-volume counters rise.
8. Observe the initial stream following terrain rather than a scripted direction.
9. Leave the area with the camera and keep simulation running.
10. Confirm via counters that source injection and frontier activity continue while it is off-screen.
11. Return and confirm the river/lake reflects elapsed simulation rather than restarting.
12. Use a source/path that reaches a depression; confirm the depression fills.
13. Confirm the lake surface rises until the actual terrain sill is reached.
14. Confirm overflow then starts and continues into a downstream channel.
15. Move away again while downstream propagation continues.
16. If terrain connectivity permits, follow the flow to ocean and confirm inland discharge without sparse ocean runaway.
17. Disable the source and verify injection stops but existing water remains and drains/settles physically.
18. Re-enable and verify the same source resumes without duplication.
19. Save the authoring state, restart/reload Planet Studio, and verify the source definition returns correctly.
20. Toggle inland/flood rendering off while simulation runs; confirm accounting continues changing.
21. Toggle rendering back on/recenter the cache; confirm current state reconstructs correctly.

### Manual acceptance gate

- [ ] Continuous m³/s source works.
- [ ] Gravity/terrain-driven stream works.
- [ ] Off-screen propagation works.
- [ ] Distant state persists.
- [ ] Basin pooling works.
- [ ] Sill overflow works.
- [ ] Downstream continuation works.
- [ ] Ocean discharge works when applicable.
- [ ] Disable/re-enable works without loss/duplication.
- [ ] Save/reload source works.
- [ ] Render visibility is independent from simulation authority.
- [ ] No obvious runaway tile allocation or unexplained mass loss.

Only after these pass may `MANUAL PLANET STUDIO ACCEPTANCE COMPLETE` be checked at the top.

---

## 13. Phase 11 — Finish and mark the task complete

<!-- PHASE_11_COMPLETION: NOT_STARTED -->

Do not enter this phase while any mandatory earlier phase is blocked or incomplete.

### Final review

Run:

```bash
git branch --show-current
git status --short
git diff --check
git log -10 --oneline
```

Review the entire plan and ensure:

- every mandatory phase marker is `COMPLETE`;
- every mandatory acceptance checkbox is `[x]`;
- all validation commands/results are recorded below;
- known non-blocking limitations are recorded explicitly;
- no unrelated changes were folded into hydrology commits;
- branch remains `pre/0.1.0`.

Then update the four top-level completion gates to `[x]` and change the top markers to exactly:

```text
<!-- ASTERRA_WATER_LONG_RANGE_STATUS: COMPLETE -->
<!-- ASTERRA_WATER_LONG_RANGE_TASK_COMPLETE: true -->
```

Also change:

```text
<!-- PHASE_11_COMPLETION: COMPLETE -->
```

Suggested final implementation commit message after all validation is complete:

```text
water: complete long-range sparse hydrology
```

The planning-only commit that creates this document must **not** set any of these completion markers.

---

## 14. Expected file ownership / change map

This is a guide, not permission to edit a stale path without checking Phase 0.

| Area | Expected responsibility in this task |
| --- | --- |
| `project.godot` | Verify active autoload/production runtime; modify only if production wiring/gating actually requires it. |
| `scripts/water/sparse_hydro_scheduler.gd` | Strengthen source-pin, wet-residency, frontier, dry-hysteresis, priority, and budget policy. |
| `scripts/water/sparse_hydrology_runtime.gd` | Authoritative sparse state, conservative activation/retirement, accounting, boundary integration. |
| Current regional hydrology/reanalysis files | Coarse/dormant persistence, reanalysis/live-state ownership, long-range regional state as appropriate. |
| `scripts/water/hydro_source_ingress.gd` | Source-to-tile lifecycle and pin/rebind behavior. |
| `scripts/water/hydro_source_terms_gpu.gd` | Continuous source application/accounting where the GPU source term is authoritative. |
| Active `scripts/water/water_system*.gd` | Public runtime/stats wiring and production gating; never camera authority. |
| `scripts/water/sparse_hydro_surface_cache_bridge.gd` | Local visible reconstruction only. |
| `scripts/water/local_water_surface.gd` | Presentation only. |
| `scripts/world_authoring/planet_studio_water_source_bridge.gd` | Existing authoring sync plus read-only diagnostics/test controls. |
| `scripts/world_authoring/model/water_point_source_definition.gd` | Preserve serialized source schema; change only if a genuinely required persistent field is missing. |
| `scripts/world_authoring/model/water_authoring_profile.gd` | Preserve source collection/save workflow. |
| Existing test directories | Add deterministic hydrology fixtures/tests according to the repo's current convention. |

---

## 15. Numerical/conservation rules that must not be violated

1. **No camera deletion:** viewer distance is not a valid reason to destroy non-zero hydrology state.
2. **No wet-cap deletion:** a hard tile cap may stall/coarsen work but cannot zero or drop wet tiles.
3. **Conservative transitions:** LOD, regional reanalysis, dormancy, and reactivation must preserve the solver's conserved quantities within tested numerical tolerance.
4. **Source accounting:** every injected volume increment must enter either stored water, intentional discharge, or measured numerical residual.
5. **Boundary accounting:** water removed at ocean/open boundaries must be counted.
6. **Deterministic frontier:** for identical initial state and inputs, neighbour activation order must not depend on camera traversal order.
7. **Rendering separation:** `LocalWaterSurface` / surface-cache state is derived presentation data, never the source of truth.
8. **No stale reanalysis overwrite:** a coarse/regional reconstruction cannot replace newer live wet state without a defined conservative reconciliation step.

---

## 16. Failure/blocker policy

If a mandatory phase cannot be completed safely:

1. change that phase marker to `BLOCKED`;
2. add a dated blocker entry under **Known blockers**;
3. keep the final task marker `false`;
4. do not substitute lossy behavior to make the demo appear complete.

Examples:

- If conservative wet-state coarsening is not supported yet, keep wet state resident rather than deleting it.
- If a test runner fails because the environment lacks Vulkan/display support, record the environment failure separately and run an appropriate supported headless path if one exists. Do not count an unexecuted test as passed.
- If regional reanalysis conflicts with live sparse authority, resolve ownership explicitly before allowing either side to overwrite water state.

---

## Audit notes

Fill this during Phase 0. Keep it concise but concrete enough for another developer/session to resume.

- Base SHA: `UNRECORDED`
- Active WaterSystem autoload: `UNRECORDED`
- Fine/sparse simulation authority: `UNRECORDED`
- Regional/reanalysis authority: `UNRECORDED`
- Source ingress path: `UNRECORDED`
- Allocation owner: `UNRECORDED`
- Retirement owner: `UNRECORDED`
- Boundary/halo exchange owner: `UNRECORDED`
- Coarsening/dormant-state owner: `UNRECORDED`
- Ocean boundary owner: `UNRECORDED`
- Viewer/render-only path: `UNRECORDED`
- Current tile budgets: `UNRECORDED`
- Current timestep/CFL/subcycling: `UNRECORDED`

## Known blockers

- None recorded yet.

## Validation log

Append entries; do not erase failed runs. Suggested format:

```text
YYYY-MM-DD | SHA | environment | command/scenario | PASS/FAIL/BLOCKED | notes
```

No validation has been run for this planning-only change.

## Completion record

- Implementation completion SHA: `UNSET`
- Validation completion SHA: `UNSET`
- Manual acceptance date: `UNSET`
- Final completion SHA: `UNSET`
