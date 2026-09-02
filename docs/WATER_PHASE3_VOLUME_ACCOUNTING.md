# Water Phase 3 — active sparse volume accounting

Branch: `water/0.1.0`

## Goal

Measure authoritative fine-SWE water without reading back tile state. This is the
missing half of representation-wide conservation accounting:

```text
coarse planetary storage + active sparse GPU storage
```

Promotion/demotion must move volume between those terms without creating or
losing water.

## GPU reduction

Implemented:

```text
shaders/water/sparse_hydro_mass_reduce.glsl
shaders/water/sparse_hydro_mass_finalize.glsl
scripts/water/sparse_hydro_volume_diagnostics_gpu.gd
```

The reducer binds:

- canonical sparse state A;
- sparse atlas occupancy;
- compact partial-volume scratch;
- one four-byte result.

`SparseHydroStepGPU` guarantees atlas A is authoritative after every candidate
substep, so the diagnostic never needs to track ping-pong parity.

The first pass maps one invocation to one sparse conservative-state cell:

```text
slot = cell_index / cells_per_tile

if occupancy[slot] == 0:
    contribution = 0
else:
    contribution = max(h, 0) * dx^2
```

Therefore recycled/unpublished slots do not count even if stale water bytes remain
in their state ranges. Each 256-thread workgroup emits one partial physical volume.
A tiny final pass sums those partials and only the four-byte total is read back
asynchronously.

No `submit()`, `sync()` or full-state readback is used.

## WaterSystem API

The production facade creates the diagnostic when the sparse runtime reaches
READY and releases it before sparse atlas teardown.

```text
WaterSystem.active_sparse_volume_diagnostic_available()
WaterSystem.request_active_sparse_volume()
WaterSystem.active_sparse_volume_ready(request_id, volume_m3)
WaterSystem.active_sparse_volume_failed(request_id, error)
```

Requests are rejected while `SparseHydrologyRuntime.busy()` is true. This avoids
calling an accounting snapshot while a macro SWE cycle is actively mutating the
canonical state.

`WaterSystem.gpu_stats()` now includes:

```text
active_sparse_volume_diagnostic
```

with capacity, tile resolution, total cells, partial count and diagnostic-owned
GPU byte count.

## Renderer gate

Scene:

```text
tests/water/SparseHydroVolumeDiagnosticsTests.tscn
```

Script:

```text
tests/water/test_sparse_hydro_volume_diagnostics.gd
```

Fixture:

```text
slot 0: h = 0.5 m
slot 1: h = 99 m   # deliberately huge stale state
slot 2: h = 1.25 m
```

The test changes only occupancy:

1. `[1, 0, 1]` — result must equal slots 0 + 2;
2. `[0, 1, 0]` — result must equal slot 1 only;
3. `[0, 0, 0]` — result must be zero despite all stale state bytes remaining.

Run with the renderer-enabled Godot 4.7 project build:

```text
godot --path . tests/water/SparseHydroVolumeDiagnosticsTests.tscn
```

The current ChatGPT environment does not contain the project Godot executable, so
this scene has not been runtime-executed here.

## Next accounting step

Add a coordinated asynchronous audit that captures:

```text
coarse_storage_m3
active_sparse_volume_m3
cumulative_outlet_m3
promotion/demotion ledgers
```

at a stable simulation boundary and checks representation-wide mass closure across
promotion, frontier expansion and eventual fine-to-coarse collapse. The audit must
not become a per-frame GPU readback path.
