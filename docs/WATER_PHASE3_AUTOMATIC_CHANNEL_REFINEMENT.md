# Water Phase 3 — Automatic channel refinement policy

Branch: `water/0.1.0`

## Status

The automatic policy is installed as the `HydroChannelRefinement` autoload but both
physical directions remain production-disabled by default:

```gdscript
WaterSystem.automatic_channel_promotion_enabled = false
WaterSystem.automatic_channel_demotion_enabled = false
```

The policy never performs a physical state mutation itself. It only selects a reach
and calls the already-transactional WaterSystem river promotion/collapse APIs.

## Promotion policy

Default scan cadence: 4 seconds.

A coarse 1D reach is eligible to enter sparse 2D refinement when either:

```text
discharge / baseline discharge >= 2.0
OR
bankfull depth ratio >= 0.85
```

Additional production constraints:

- generated macro river only;
- minimum stream order 2;
- not already refined;
- not in per-cell retry/post-collapse cooldown;
- global refined-reach count below the default cap of 8;
- river promotion bridge and continuous coupling available;
- one ownership transaction at a time.

Candidates are queried only down to the lower hysteresis band, then filtered by the
higher enter thresholds. Ranking combines:

```text
hydraulic anomaly severity
+ stream-order bonus
+ upstream drainage/confluence indegree bonus
```

The indegree cache is built once per coarse-store generation from the persistent
receiver graph, so scan work remains bounded by the candidate list rather than
searching the planet for every candidate.

The policy delegates the actual transfer to:

```text
WaterSystem.promote_coarse_river_reach()
  -> channel-only ownership reservation
  -> terrain-aligned GPU corridor reconstruction
  -> exact GPU acknowledgement
  -> coarse channel debit
  -> continuous 1D<->2D coupling registration
```

Only reaches successfully promoted by this automatic policy are recorded in the
policy's auto-owned registry. Manual river promotions are never auto-collapsed.

## Collapse hysteresis

Collapse is evaluated before new promotion on each scan, freeing an unnecessary GPU
reach before asking for another one.

The fine corridor must first pass the existing river collapse policy:

```text
SETTLING / FROZEN
quiet >= 20 s
max velocity <= 0.01 m/s
max outgoing flux <= 0.01 m3/s
disturbance energy <= 5e-5
```

The hydraulic anomaly must also have exited the lower hysteresis band:

```text
measured fine downstream Q / baseline Q < 1.35
AND
coarse residual bankfull ratio < 0.60
```

This dual test prevents a visually quiet fine tile from collapsing while the hybrid
river is still carrying an abnormal through-flow or while the residual 1D segment
remains close to bankfull.

The measured downstream Q comes from the continuous fine mouth coupling, not from a
camera estimate or a reconstructed coarse guess.

Successful reverse selection delegates to:

```text
WaterSystem.collapse_fine_river_reach()
  -> suspend mouth coupling
  -> 16-byte fine state reduction
  -> channel-only incoming ownership reservation
  -> fine unpublish
  -> exact coarse channel commit
  -> pending confluence inflow return
  -> refinement-hole removal
  -> pure 1D routing resumes
```

## Cooldowns and capacity

Defaults:

```text
retry after rejected/failed transaction: 3 scans
post-collapse re-refinement cooldown:     5 scans
maximum total refined reaches:            8
one transaction started per scan:         yes
```

The cap uses the store's total refined reach count, including manual refinements.
This is intentionally conservative: manually pinned/high-detail river domains consume
real sparse capacity and therefore reduce automatic refinement headroom.

## Camera independence

No observer/camera/visual-cache quantity participates in candidate selection,
hysteresis, priority, cooldown or collapse.

The representation exists because of hydraulic state:

```text
1D normal river
   -> abnormal hydraulic state
   -> sparse 2D refinement
   -> continuous hybrid flow
   -> quiet + hydraulic exit hysteresis
   -> persistent 1D collapse
```

Rendering remains a disposable consumer of whichever physical representation owns
the water.

## Validation gates

CPU/headless policy gate:

```text
godot --headless --path . tests/water/HydroAutomaticChannelRefinementTests.tscn
```

It checks enter/exit hysteresis, anomaly ranking, stream-order/confluence priority,
and collapse exit conditions.

Renderer-mode full automatic round trip:

```text
godot --path . tests/water/HydroAutomaticChannelRefinementGPU.tscn
```

The renderer gate uses the real:

- coupled persistent river store;
- sparse atlas;
- terrain-aligned river promotion bridge;
- continuous coupling registry;
- quiet river collapse bridge;
- active sparse volume reduction.

It explicitly enables the policy switches only inside the fixture, promotes one
high-Q reach through `scan_once()`, transitions it to a quiet below-exit state, then
calls the same policy again and requires the automatic collapse to return:

```text
coarse_final == coarse_initial
surface storage unchanged
channel storage restored
active fine volume == 0
promoted ownership == demoted ownership
```

The current ChatGPT environment does not contain the project's Godot 4.7 executable,
so these gates are implemented but not runtime-passed here.

## Next boundary

After local runtime validation, the next policy-scale work should be **multi-tile
river refinement clusters** rather than simply increasing the automatic cap:

- upstream/downstream neighboring refined reaches;
- confluence clusters sharing a junction;
- cluster-level promotion/collapse transactions;
- fine-fine internal reach boundaries instead of repeated 1D mouth exchanges;
- cluster hysteresis so one segment cannot chatter independently inside an active
  refined river system.
