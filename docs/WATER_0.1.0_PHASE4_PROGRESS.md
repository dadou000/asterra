# Water 0.1.0 — Phase 4 physical HydroLOD progress

Branch: `water/0.1.0`

## Production status

Phase 4 now has a conservative spatial 2:1 HydroLOD path. The production `WaterSystem`
autoload is `water_system_hydrolod_production.gd`.

### Physical hierarchy and metrics

- H0 is the configured finest sparse quadtree level.
- H1/H2/... are true quadtree parents with `dx(Hn) = dx(H0) * 2^n`.
- `HydroTilePool.topology_revision` tracks allocation/publication changes.
- ordinary allocations reject overlapping parent/descendant ownership.
- atomic LOD transitions may temporarily reserve the exact representation they are
  replacing while runtime is paused.
- sparse SWE, CFL, activity diagnostics and source-volume accounting use each tile's
  actual physical cell size/area.

### Conservative parent/children transfer

Implemented GPU restriction and prolongation:

```text
4 children -> parent
    conservative area restriction of h/hu/hv

parent -> 4 staged children
    terrain-aware water filling
    exact parent parcel distributed over child bathymetry
```

The ownership swap is transactional and rollback-capable. River/component refined
members are pinned and cannot be silently moved by generic HydroLOD policy.

### Live 2:1 coarse/fine interfaces

Implemented:

```text
scripts/water/hydro_lod_interface_flux_gpu.gd
shaders/water/hydro_lod_interface_flux.glsl
scripts/water/sparse_hydro_step_gpu_lod.gd
scripts/water/hydro_physical_lod_manager_interfaces.gd
```

`HydroLODInterfaceFluxGPU` builds a compact descriptor table from authoritative
`HydroTilePool` ownership. Each descriptor represents one coarse edge against up to
two immediate fine neighbor tiles. Partial sparse boundaries are supported; an
unrepresented half remains reflective.

The registry validates a strict 2:1 balance. A touching descendant deeper than one
level or a covering neighbor more than one level coarser fails closed before another
physical step can be submitted.

Each adaptive CFL substep now records:

```text
reduce canonical A using local dx
    -> prepare globally safe dt
    -> ordinary sparse SWE A -> B
       (mixed edges temporarily reflective)
    -> 2:1 interface correction
       read the same pre-step A
       remove reflective boundary contribution from B
       apply one physical coarse/fine flux to both sides
    -> canonicalize corrected B -> A
```

The interface kernel:

- works in a common coarse outward-normal/tangent frame;
- transforms fine momentum through cube-face seam orientation;
- maps one coarse edge cell to two fine edge cells;
- integrates exactly the same water parcel with opposite sign on coarse and fine
  representations;
- replaces, rather than adds to, the reflective boundary impulse from the ordinary
  solver;
- applies a donor-water positivity limiter to the shared physical parcel;
- serializes interface descriptors with GPU barriers so tile-corner corrections do
  not race.

This is the same-step reflux path: there is no independent coarse/fine flux estimate
left to drift apart spatially.

### Frontier and source integration

- Mixed-resolution edges are claimed by the interface registry before frontier
  reachability/allocation, preventing the frontier path from trying to allocate an
  overlapping same-level tile.
- Interface descriptors are invalidated by `HydroTilePool.topology_revision` and are
  synchronized before the next physical step.
- Point sources follow the active quadtree owner of their physical location. A
  nominal H0 source therefore remains active while the region is represented by an
  H1/H2 parent.
- Point-source m3/s -> depth-rate conversion uses the resolved owner's physical cell
  area.

### Transactional topology publication

`HydroPhysicalLODManagerInterfaces` treats both topology mirrors as part of commit:

```text
ownership swap
    -> same-level connectivity publish
    -> 2:1 interface descriptor publish
    -> success / runtime resume
```

If either topology publication rejects the result, the existing conservative
parent/children rollback path runs before simulation resumes.

## Validation status

The current ChatGPT execution environment still does not provide the project's Godot
4.7 renderer executable. These changes have therefore been statically integrated but
not claimed as renderer/runtime-passed.

No temporary test files were added in this implementation slice.

## Next Phase-4 slice

The remaining major limitation is temporal. All active levels currently use the
finest global CFL step. The next production implementation is physical HydroLOD
subcycling:

```text
H0: dt
H1: 2*dt when locally safe
H2: 4*dt when locally safe
...
```

That requires accumulated interface flux registers so fine substeps can reflux the
coarse owner at synchronization points without changing total water. After temporal
subcycling is stable, automatic HydroLOD refine/coarsen policy can be enabled with
hysteresis, disturbance pinning and capacity pressure.
