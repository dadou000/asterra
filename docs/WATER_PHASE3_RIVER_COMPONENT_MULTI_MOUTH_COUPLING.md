# Water 0.1.0 Phase 3 — River Component Multi-Mouth Coupling

## Purpose

Cross-macro-reach component unions now have a production coupling path when their
fine representation is physically continuous and their coarse ownership can be
bypassed without skipping an authoritative residual 1D segment.

The component model is:

```text
external/local coarse queues          external/local coarse queues
             |                                     |
             v                                     v
      [ fine tributary A ]                  [ fine tributary B ]
                \                                  /
                 \---- normal sparse SWE --------/
                              |
                         [ fine stem ]
                              |
                              v
                    one downstream 2D -> 1D mouth
```

There may be multiple coarse-owned upstream/local injections, but there is exactly
one downstream fine-to-coarse removal mouth for the connected component.

## Why the coupling switch is conditional

The component registry is metadata-only. Two refined reaches cannot safely bypass
their legacy 2D -> residual-1D -> 2D handoff merely because their coarse receiver
cells are connected.

`HydroRiverComponentCouplingContract` therefore requires every internal edge to
satisfy all of the following before component mode activates:

1. The upstream reach's `residual_length_m` is effectively zero.
2. The upstream reach has no remaining `channel_storage_m3` residual parcel.
3. The final sparse member of the upstream cluster is a cardinal
   `HydroTileTopology` neighbor of the receiver cluster's first sparse member.
4. Every component member remains an active refined reach with a valid sparse
   cluster identity.

If any condition fails, the component remains registered but coupling uses the
existing per-reach mouth path. No ownership is moved merely to make the component
eligible.

This is deliberately conservative: a logical union can exist before its physical
fine topology is ready.

## Component exchange planning

`HydroRiverComponentExchangePlanner` builds the compact GPU record batch.

Each coarse reach retains its own `refined_pending_inflow_m3` queue. Therefore even
a non-root component reach may need an upstream injection record for local lateral
runoff or external coarse donors.

For every component reach:

```text
requested_add = min(pending_coarse_volume, refined_inflow_rate * dt)
```

If `requested_add > 0`, the reach's first sparse member receives an upstream-enabled
record.

Only `downstream_outlet_cell` receives a downstream-enabled record. Internal reach
downstream records are omitted because sparse SWE crosses the validated fine/fine
interfaces directly.

For a one-member outlet with pending inflow, the same 64-byte GPU record carries both
flags:

```text
upstream_enabled   = true
downstream_enabled = true
```

This avoids two compute invocations writing the same sparse slot.

## GPU ABI

No shader ABI expansion was required. `HydroRiverReachExchangeGPU` already supports
per-record mouth flags:

```text
bit 0 = upstream 1D -> 2D enabled
bit 1 = downstream 2D -> 1D enabled
```

A component dispatch can therefore contain, for example:

```text
tributary A: upstream=true  downstream=false
tributary B: upstream=true  downstream=false
main stem:   upstream=true  downstream=true
```

All records own distinct sparse slots. The existing shader remains one invocation per
record and requires no atomics for this boundary exchange.

## Acknowledgement and ownership

GPU results are still aggregated by coarse reach cell before any ledger mutation.

For each reach:

- acknowledged `added_m3` consumes only that reach's pending coarse inflow;
- only the component outlet can acknowledge `removed_m3`;
- outlet removal is credited to that outlet reach's residual coarse channel storage;
- internal fine/fine flux is not entered into either promotion/demotion ledger.

The coupling transaction still pauses both the sparse runtime and coarse persistent
owner while compact GPU acknowledgement is pending.

A bad GPU status or identity mismatch fails closed with both owners paused.

## Conservative fallback

`HydroRiverReachClusterCoupling` checks the component contract on every exchange
cycle.

If the component is not ready, it emits the original per-reach boundary records:

```text
fine reach A -> residual A 1D -> pending B -> fine reach B
```

This is slower and less purely fine-grained, but it preserves the already-tested
ownership model while physical component construction catches up.

Diagnostics report the most recent fallback reason, including:

```text
component_internal_reach_has_residual_1d
component_internal_reach_has_residual_water
component_internal_fine_gap
component_coupling_record_missing
```

## Collapse safety

Individual `suspend_reach(cell)` is now rejected while the cell belongs to a refined
component:

```text
ERR_BUSY: reach_belongs_to_refined_component
```

Suspending one branch while the other component mouths remain live would invalidate
the component-wide boundary contract. Component-level collapse remains the next
required ownership transaction.

## Diagnostics

Component-aware exchange reports add:

```text
component_multi_mouth_exchange
component_count
component_fallback_count
component_boundary_record_count
component_injection_record_count
component_external_upstream_mouth_count
```

Coupling stats add:

```text
component_multi_mouth_coupling = true
registered_refined_components
coupling_ready_refined_components
last_component_fallback_reason
```

## Validation gates

CPU contract and record planner:

```text
godot --headless --path . tests/water/HydroRiverComponentExchangePlannerTests.tscn
```

This gate verifies:

- two tributaries plus one main stem become coupling-ready when both internal
  upstream reaches are fully represented and fine boundaries are cardinally joined;
- three coarse pending queues produce three injection records;
- exactly one downstream removal record exists;
- a one-member outlet combines upstream/downstream flags safely;
- planning does not mutate coarse ownership;
- residual internal 1D coverage forces legacy fallback;
- a physical fine gap forces legacy fallback.

Renderer/GPU multi-mouth ABI:

```text
godot --path . tests/water/HydroRiverComponentBoundaryExchangeGPU.tscn
```

The renderer gate submits two upstream-only records and one combined
upstream+downstream outlet record in a single dispatch, then checks exact acknowledged
additions and final per-slot volume closure.

The current assistant environment does not contain the project Godot executable, so
these scenes are committed as validation gates but have not been runtime-executed
here.

## Next implementation slice

The coupling path is now able to use a physically continuous component, but automatic
construction of that topology is still missing.

Next:

1. component promotion/extension transaction that fills fine gaps and fully promotes
   internal reaches without exposing partial topology;
2. confluence-aware sparse corridor seeding/junction geometry;
3. automatic component union only after the complete fine topology is publishable;
4. component-level GPU reduction and atomic collapse back to all coarse ownership
   parcels;
5. only then consider enabling automatic cross-reach refinement policy.
