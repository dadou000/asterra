# Water 0.1.0 Phase 3 — River Component Multi-Mouth Coupling

## Purpose

Cross-macro-reach component unions now have a production coupling path when their
fine representation is physically continuous and their coarse ownership can be
bypassed without skipping an authoritative residual 1D segment.

The compact GPU ABI is also proven for a future true confluence batch with multiple
upstream additions and one downstream outlet. Production activation of a **multi-root
confluence**, however, remains deliberately gated until branched fine-junction
geometry is seeded and verified.

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
4. The two seeded corridor centerlines actually cross that shared edge at compatible
   positions and orientations.
5. Every component member remains an active refined reach with a valid sparse
   cluster identity.

The corridor test uses the exact cube-face seam orientation from
`HydroTileTopology`, maps the source edge parameter into the destination edge frame,
and compares the two centerline crossings using a tolerance derived from the two
river half-widths and the sparse cell size.

If any condition fails, the component remains registered but coupling uses the
existing per-reach mouth path. No ownership is moved merely to make the component
eligible.

## Multi-root junction safety

A real confluence is stricter than ordinary cross-tile continuity.

```text
tributary A ----\
                 >---- main stem
tributary B ----/
```

The current `HydroRiverCorridorProlongationGPU` seeds one straight corridor per tile.
A single straight centerline cannot in general represent a Y-shaped junction with
two independent incoming edges and one outgoing branch.

Therefore a component with:

```text
upstream_mouth_count > 1
```

is currently rejected by the production coupling contract with:

```text
component_fine_junction_not_verified
```

until a future component promotion/junction builder publishes verified branched
junction geometry. This prevents tile adjacency from being mistaken for hydraulic
continuity.

## Component exchange planning

`HydroRiverComponentExchangePlanner` builds the compact GPU record batch for a
coupling-ready component.

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

The renderer gate proves that one dispatch can safely contain:

```text
tributary A: upstream=true  downstream=false
tributary B: upstream=true  downstream=false
main stem:   upstream=true  downstream=true
```

All records own distinct sparse slots. The existing shader remains one invocation per
record and requires no atomics for this boundary exchange.

This proves the **boundary ABI** needed by the coming junction builder; it does not
by itself claim that independently seeded straight corridors form a valid confluence.

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

Diagnostics may report:

```text
component_internal_reach_has_residual_1d
component_internal_reach_has_residual_water
component_internal_fine_gap
component_internal_corridor_edge_miss
component_internal_corridor_wrong_orientation
component_internal_corridor_gap
component_fine_junction_not_verified
component_coupling_record_missing
```

## Collapse safety

Individual `suspend_reach(cell)` is rejected while the cell belongs to a refined
component:

```text
ERR_BUSY: reach_belongs_to_refined_component
```

Suspending one branch while the other component mouths remain live would invalidate
the component-wide boundary contract. Component-level collapse remains a required
follow-up transaction.

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

- a fully represented linear upstream reach can bypass its coarse residual when the
  two fine corridors meet continuously across the shared sparse edge;
- each coarse pending queue still receives its own injection record;
- exactly one downstream removal record exists for the component;
- a one-member outlet safely combines upstream/downstream flags;
- planning does not mutate coarse ownership;
- residual internal 1D coverage forces legacy fallback;
- non-neighbor sparse tiles force fallback;
- adjacent tiles with mismatched corridor geometry force fallback;
- a two-tributary confluence remains gated until branched junction seeding is
  explicitly verified.

Renderer/GPU multi-mouth ABI:

```text
godot --path . tests/water/HydroRiverComponentBoundaryExchangeGPU.tscn
```

The renderer gate submits two upstream-only records and one combined
upstream+downstream outlet record in one dispatch, then checks acknowledged additions
and final per-slot volume closure.

The current assistant environment does not contain the project Godot executable, so
these scenes are committed as validation gates but have not been runtime-executed
here.

## Next implementation slice

The boundary/coupling layer is now ready for physically valid cross-reach components.
The next missing piece is the **component promotion and branched junction builder**:

1. add a junction corridor representation capable of multiple incoming segments and
   one outgoing segment inside a sparse tile;
2. stage/seed the complete component while still hidden;
3. fully promote internal coarse reaches in one ownership-safe transaction;
4. publish the complete sparse component and only then mark its fine junction
   geometry verified;
5. add component-level GPU reduction and atomic collapse back to all coarse ownership
   parcels;
6. only then consider enabling automatic cross-reach confluence refinement.
