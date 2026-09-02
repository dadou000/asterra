# Water 0.1.0 Phase 3 — River Component Unions

## Purpose

The first river-cluster implementation refines one coarse macro reach into one ordered
sparse SWE cluster. That is sufficient for a single reach, but a real confluence may
need several refined coarse reaches to participate in one connected fine component.

This change installs the **CPU-side transactional topology contract** for that next
step. It deliberately does not yet change GPU coupling or move water between existing
ownership parcels.

## Component model

`PlanetHydrologyRiverClusterStore` now maintains two additional indices:

```gdscript
_refined_components     # component_id -> component record
_component_by_cell      # coarse reach cell -> component_id
```

A component contains two or more already-refined coarse reaches. Each coarse reach
keeps its existing ownership hole and its own represented fine volume. The component
only states that those reach clusters are intended to behave as one directed fine
river component.

The topology contract is:

```text
zero or more upstream refined branches
              \
               -> connected directed component -> exactly one downstream outlet
              /
```

For the confluence case, multiple upstream mouths are expected. A component must have
exactly one downstream coarse outlet.

## Atomic union operation

The store exposes:

```gdscript
merge_refined_clusters(cells: PackedInt32Array)
```

The operation is transactional at the registry level:

1. Every requested cell must already be a refined river reach.
2. If a requested cell already belongs to a component, that entire component is
   expanded into the candidate set.
3. All source components are expanded before validation.
4. The receiver graph is validated before registry mutation.
5. Every sparse tile identity and scheduler slot must be unique inside the component.
6. Only after validation succeeds are old component records removed and the new union
   published.

A failed merge therefore leaves the previous component registry unchanged.

## Directed graph validation

The union builder derives internal edges directly from the authoritative coarse river
receiver graph.

It requires:

- at least two refined reaches;
- exactly one reach whose receiver leaves the component;
- at least one zero-indegree upstream root;
- an acyclic receiver graph;
- all nodes to be visited by the deterministic Kahn topological pass.

Because every coarse reach has one downstream receiver, a complete acyclic traversal
with exactly one external outlet is also a connected single-outlet drainage component.

The component record publishes:

```text
cells
reach_count
upstream_mouth_cells
upstream_mouth_count
downstream_outlet_cell
downstream_receiver
internal_reach_edges
fine_tile_ids
fine_slots
fine_member_count
topology = directed_single_outlet_confluence
representation = sparse_2d_river_component
ownership_changed = false
```

## Ownership boundary

Component union is **metadata-only**.

It does not:

- debit additional coarse channel storage;
- credit fine storage;
- consume pending inflow;
- return fine storage to coarse;
- change the promotion/demotion ledger.

`dissolve_refined_component(component_id)` removes only this topology metadata and
leaves every individual refined reach intact.

To prevent topology from being invalidated underneath a future multi-mouth coupling
transaction, an individual component member cannot currently be unregistered:

```text
ERR_BUSY: reach_belongs_to_refined_component
```

The future component-level collapse bridge must first collapse/dissolve the connected
component conservatively.

## Sparse identity protection

`register_refined_cluster()` now rejects a tile or scheduler slot already owned by
another registered refined river cluster. This keeps component records unambiguous
until shared physical confluence tiles are introduced through a dedicated component
promotion transaction rather than accidental double registration.

This means the current component union describes connected **logical** reach clusters;
it does not yet make two independently promoted reaches share one physical sparse tile.

## Diagnostics

`PlanetHydrologyRiverClusterStore.stats()` now includes:

```text
river_refined_components
component_refined_reaches
component_sparse_members
multi_upstream_mouth_components
max_component_upstream_mouths
component_union_transactional = true
component_union_changes_ownership = false
```

## Validation gate

CPU/headless component registry gate:

```text
godot --headless --path . tests/water/PlanetRiverClusterComponentsTests.tscn
```

The test covers:

- a two-tributary + main-stem confluence forming one component with two upstream
  mouths and one downstream outlet;
- per-reach unregister being blocked while the component contract is active;
- dissolving component metadata without changing individual refined ownership;
- atomically unioning two existing components;
- rejecting a disconnected/multi-outlet candidate without registry residue.

## Next implementation slice

The next step is **component-aware multi-mouth fine coupling**:

1. promote/build a physically connected sparse confluence topology;
2. map every external refined donor branch to an upstream fine mouth;
3. expose one downstream fine mouth for the component;
4. exchange all boundary records in one coupling transaction;
5. aggregate acknowledgements per coarse ownership parcel;
6. add component-level collapse/readback so the whole connected fine component can be
   returned conservatively.

Automatic cross-reach promotion should remain disabled until that GPU path has its own
mass-conservation and seam/confluence validation gates.
