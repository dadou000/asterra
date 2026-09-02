# Water 0.1.0 — Generic hydrology sources

Branch: `water/0.1.0`

## Purpose

The sparse hydrology runtime now has a generic conservative ingress/egress path for
localized water sources:

```text
glacier melt
spring
reservoir outlet
pipe / pump
drain
breach discharge
gameplay emitter
       |
       v
HydroSourceIngress
       |
       +--> create first tile if Q > 0 and absent
       +--> GPU terrain reconstruction
       +--> compact source-term rebuild
       v
SparseHydrologyRuntime
       v
connected sparse SWE / frontier expansion
```

Rainfall and watershed runoff will **not** be represented as thousands of point
sources. They will later feed a distributed precipitation/soil/runoff field into the
same conservative source buffer/interface.

## API

Production `SparseHydrologyRuntime` exposes:

```gdscript
upsert_point_source(
    source_id: String,
    direction: Vector3,
    rate_m3_s: float,
    injection_velocity_world: Vector3 = Vector3.ZERO,
    tile_level: int = -1,
    source_enabled: bool = true,
)

remove_point_source(source_id)
set_point_source_enabled(source_id, enabled)
```

`rate_m3_s` is signed:

- `Q > 0`: add water;
- `Q < 0`: remove water;
- `Q = 0`: no source term.

A positive source may allocate the first hydrology tile. A sink never allocates a
missing dry tile.

## Conservative cell source contract

The sparse source buffer remains one `vec4` per active cell:

```text
x = added depth rate     [m/s]
y = removed depth rate   [m/s]
z = injected hu rate     [m²/s²]
w = injected hv rate     [m²/s²]
```

A volumetric source with flow `Q` entering one solver cell of area `A` becomes:

```text
dh/dt = Q / A
```

For an incoming tangent velocity `(u,v)` in the tile's local face frame:

```text
d(hu)/dt = (Q/A) * u
d(hv)/dt = (Q/A) * v
```

`HydroSourceIngress` accepts a world-space velocity. Its radial component is removed
and the tangent component is projected onto the local equi-angular cube-face `+u`
and `+v` directions before the compact source entry is built.

## Sink momentum behavior

A sink cannot merely subtract `h` while leaving `hu/hv` unchanged; doing so would
increase velocity and manufacture kinetic energy.

The SWE source update therefore removes a proportional local fluid parcel:

```text
h_after = max(h_before - dh_remove, 0)
q_after = q_before * (h_after / h_before)
```

where `q = (hu,hv)` after any incoming source contribution. This models a drain,
infiltration or extraction term carrying away the local water's momentum.

## Compact GPU update

Added:

```text
scripts/water/hydro_source_terms_gpu.gd
shaders/water/hydro_source_terms.glsl
```

CPU policy does **not** upload a source grid. It resolves/aggregates active point
sources by `(slot, local_cell)` and uploads one 32-byte entry per affected cell:

```text
slot
local cell index
add depth rate
remove depth rate
hu rate
hv rate
```

The GPU pass clears the source buffer and writes only those compact entries.
Duplicate point emitters in the same cell are aggregated before upload, avoiding
floating-point atomics.

The default entry capacity is 8192, approximately 256 KiB of staging storage.

## First-domain creation

A positive source on an absent tile uses the same safe lifecycle as frontier
expansion:

```text
source definition
      |
      v
resolve HydroTileKey + cell
      |
      v
reserve tile -> ALLOCATING / GPU occupancy=0
      |
      v
HydroTerrainBedGPU stage
      |
      v
terrain stage recorded
      |
      v
activate_reserved
      |
      v
publish identity + occupancy
      |
      v
sync connectivity
      |
      v
rebuild compact source buffer
```

A source therefore never exposes stale recycled water/bed data to the solver.

## Source synchronization and runtime ordering

`SparseHydrologyRuntime` has an explicit `SOURCES` phase.

Source definitions can be changed at any time, but GPU source-buffer rebuild and
first-tile creation occur only when the runtime reaches an IDLE boundary:

```text
IDLE
  -> source dirty?
       -> SOURCES
       -> flush / tile creation / compact GPU rebuild
       -> IDLE
  -> SWE advance
```

This prevents source buffer clears/updates from racing a recorded SWE command list.

Source definitions have a monotonic revision. If a source changes while a previous
sync is still in flight, the older callback cannot clear the newer dirty state; the
runtime schedules another source sync at the next IDLE boundary.

## Test gate

Added:

```text
tests/water/HydroSourceIngressSmoke.tscn
tests/water/test_hydro_source_ingress.gd
```

The test starts with **zero resident hydrology tiles** and deliberately stale state
in unused GPU slots. At the centre of the +X cube face it defines:

```text
spring A: +2.0 m³/s, local velocity (+1.5, -0.5)
spring B: +1.0 m³/s, local velocity (-0.5, +0.5)
drain:    -0.5 m³/s
```

All three occupy one cell. The gate verifies:

1. the positive sources create exactly one sparse tile;
2. GPU terrain staging overwrites stale slot state;
3. the compact writer produces exactly one aggregated source entry;
4. source rates equal:

```text
add depth = 3.0 m/s
remove depth = 0.5 m/s
hu rate = +2.5 m²/s²
hv rate = -0.5 m²/s²
```

for the 1 m² fixture cell;
5. one 0.10 s SWE step produces the exact expected source/sink depth and momentum;
6. removing all source definitions clears the GPU source buffer.

Suggested renderer-mode run:

```text
godot --path . tests/water/HydroSourceIngressSmoke.tscn
```

The current ChatGPT execution environment does not contain the project Godot 4.7
renderer executable, so this gate is implemented but not runtime-passed here.

## Why WaterSystem does not auto-bootstrap sparse hydrology yet

`WaterSystem` remains conservative about production activation. The sparse runtime
requires a fail-closed boundary reachability policy before arbitrary world sources
can safely cause frontier expansion.

Automatically creating the runtime with a permissive callback would make this wrong:

```text
wet tile adjacent to cliff/levee/building
        -> topology says neighbor exists
        -> permissive bootstrap says reachable
        -> flood incorrectly wakes/passes boundary
```

The next integration therefore needs a reusable terrain/structure reachability
service. Once that exists, `WaterSystem` can own/bootstrap `SparseHydrologyRuntime`
and expose the generic source API directly to weather, river, glacier and gameplay
systems.
