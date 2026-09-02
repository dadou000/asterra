# Water Phase 3 — continuous 1D ↔ sparse 2D river coupling

Branch: `water/0.1.0`

## Scope

This checkpoint closes the through-flow gap left by the initial river corridor
promotion seed. Explicitly promoted macro river reaches now participate in a
continuous operator-split coupling loop between the persistent 1D drainage network
and sparse 2D SWE.

Automatic channel promotion remains disabled.

## Coarse representation split

Production now uses `PlanetHydrologyRiverCoupledStore`, derived from the channel-
aware reach ownership store.

A promoted macro reach is split conceptually into:

```text
upstream donors / confluence
        |
        v
pending coarse-owned inflow queue
        |
        | GPU acknowledgement
        v
sparse 2D river node/corridor
        |
        | actual measured downstream parcel
        v
downstream residual 1D reach
        |
        v
normal coarse receiver routing
```

The fine corridor is treated as the upstream/node part of the coarse reach. The
remaining channel storage is advanced using the same Manning cross-section but with
its residual physical length. This preserves finite travel time outside the refined
section instead of continuing to use the original full reach length.

## Donor and confluence fan-in

The existing deferred one-hop coarse routing buffer remains authoritative for donor
outflow. When the receiver cell is refined, incoming donor volumes are diverted into
`refined_pending_inflow_m3` rather than being added to the residual coarse channel.

Multiple donors therefore fan into one conservative queue naturally:

```text
tributary A ----\
                 +--> pending fine inflow --> 2D node
tributary B ----/
```

The fine share of local lateral channel input (baseflow, surface-to-channel release,
and climatology fallback) is also diverted according to represented reach-length
fraction. The residual share remains in the downstream 1D segment.

Pending inflow remains part of coarse total storage until the GPU confirms that the
parcel was written into the fine corridor.

## GPU mouth exchange

Added:

```text
shaders/water/hydro_river_reach_exchange.glsl
scripts/water/hydro_river_reach_exchange_gpu.gd
```

One GPU invocation owns one promoted reach record.

At the upstream corridor mouth:

- a bounded queued 1D parcel is distributed across the corridor width;
- the parcel receives the reach's downstream local tangent velocity;
- the exact FP32 volume actually written is returned to CPU.

At the downstream mouth:

- the kernel measures positive advective discharge from the current conservative
  `hu/hv` state;
- it removes the corresponding parcel for the completed fine macro timestep;
- sink clipping is exact because removal is limited by water physically present;
- the exact removed volume and measured discharge are returned.

A and B are edited identically so atlas A remains canonical.

The compact result per reach is only 16 bytes:

```text
added_1d_to_2d_m3
removed_2d_to_1d_m3
measured_downstream_q_m3s
mouth/status
```

No full water-grid readback is introduced.

## Ownership accounting

`PlanetHydrologyRiverCoupledStore.consume_refined_inflow()` removes only the volume
acknowledged by the GPU and increments the inherited coarse→fine ownership ledger.

`accept_refined_outflow()` adds only the GPU-measured removed parcel to the residual
1D channel and increments the inherited fine→coarse ledger.

Therefore through-flow is a representation transfer, not an environmental source:

```text
coarse pending --V--> fine
fine           --W--> coarse residual
```

The environmental fine-source ledger is intentionally unaffected.

## Stable cycle boundary

`HydroRiverReachCoupling` listens to `SparseHydrologyRuntime.cycle_completed`.
The callback occurs after the adaptive SWE macro step and while the runtime is IDLE.
The coupling temporarily disables both sparse and coarse advancement, records the
compact GPU mouth exchange, reconciles its result with the coarse ownership store,
then restores both prior enabled states and lets sparse time debt continue.

If the GPU exchange result cannot be reconciled, the controller fails closed and
leaves both owners paused rather than continuing with ambiguous mass ownership.

## Production integration

`WaterSystem` now resolves through:

```text
scripts/water/water_system_river_coupled.gd
```

Successful explicit `promote_coarse_river_reach()` reports are automatically
registered with the continuous coupling controller.

Surface promotion/demotion and new river promotion requests are rejected while a
river mouth exchange is pending. Sparse generation teardown is deferred until an
in-flight exchange has reconciled.

`automatic_channel_promotion_enabled` remains `false`.

## Gates

CPU/headless:

```text
godot --headless --path . tests/water/PlanetRiverRefinedCouplingTests.tscn
```

The fixture creates two donor cells feeding one refined river node and checks:

- donor debit equals the pending refined inflow queue;
- multiple donors aggregate conservatively;
- GPU-acknowledged queue consumption changes only representation ownership;
- fine outflow credit enters the residual coarse channel;
- coarse + hypothetical fine volume remains invariant;
- the coarse ownership mass error remains zero.

Renderer/GPU:

```text
godot --path . tests/water/HydroRiverReachExchangeGPU.tscn
```

The fixture checks:

- exact upstream addition;
- positive measured downstream discharge;
- actual downstream parcel removal;
- A/B identity;
- dry off-corridor cells remain dry;
- final fine volume equals initial + added - removed.

These scenes are implemented but not runtime-passed in the current ChatGPT
environment because the project Godot 4.7 renderer executable is unavailable.

## Remaining river work

1. Add a renderer end-to-end test using the real persistent store + promotion bridge
   + sparse runtime + continuous coupling controller in one fixture.
2. Add quiet-river sparse 2D -> 1D collapse that reconstructs residual reach storage
   and returns any pending coupling parcel exactly.
3. Add refined-reach save/world-transition flushing instead of blocking coarse-only
   snapshots while river refinements exist.
4. Only after the continuous/collapse gates pass locally, add a low-cadence automatic
   channel promotion/deactivation policy.
