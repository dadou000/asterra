# Water Phase 3 — exact fine external-flux accounting

Branch: `water/0.1.0`

## Purpose

Sparse SWE now owns real environmental water sources: distributed weather forcing,
point/gameplay emitters and sinks. Representation-wide conservation therefore needs
more than coarse storage + active fine storage + outlet export; it must also know
what water was actually added to or removed from fine SWE.

Requested source rates are not sufficient for sinks because a drain can request
more water than a cell contains. The ledger must record the **applied** sink parcel.

## GPU source ledger

`shaders/water/sparse_hydro_step.glsl` now owns one `vec2` ledger entry per sparse
state cell:

```text
x = gross external water added   [m3]
y = gross external water removed [m3]
```

The buffer is cleared before each macro advance. During every active CFL substep,
exactly one invocation owns each cell, so it can accumulate its own ledger entry
without float atomics.

For the composed gameplay + atmospheric source term:

```text
add_h = max(source.add_rate, 0) * dt
available_after_add = max(h_after_flux + add_h, 0)
actual_remove_h = min(requested_remove_h, available_after_add)
```

The physical ledger increment is:

```text
(add_h, actual_remove_h) * dx^2
```

This deliberately records source mechanics before the later positivity/invalid-state
clamp. Any numerical mass correction caused by the SWE scheme therefore remains
visible as a conservation residual instead of being hidden inside the source ledger.

## Compact reduction

Added:

```text
shaders/water/sparse_hydro_external_flux_reduce.glsl
shaders/water/sparse_hydro_external_flux_finalize.glsl
```

The first pass reduces per-cell `vec2` ledger entries to one `vec2` per 256-thread
workgroup. The final pass sums those partials and writes FP32 bit patterns into the
two previously reserved words of the existing 96-byte sparse solver control block:

```text
byte 88 = external_added_m3
byte 92 = external_removed_m3
```

No extra production CPU readback is introduced. `SparseHydroStepGPU` exposes:

```text
external_added_m3
external_removed_m3
external_net_m3
external_sink_clipping_exact = true
```

inside the normal `diagnostics_ready` dictionary.

## Persistent fine ledger

Added autoload:

```text
HydroFineExternalFluxLedger
```

implemented by:

```text
scripts/water/hydro_fine_external_flux_ledger.gd
```

It listens to the production sparse solver's diagnostics and accumulates:

```text
cumulative_added_m3
cumulative_removed_m3
cumulative_net_m3
```

across ordinary sparse runtime recycling. It resets only when
`PersistentHydrologySystem.store_rebuilt` establishes a new world-hydrology
generation.

## Representation audit

`HydroRepresentationAudit` now automatically consumes the fine ledger when it is
present and complete.

The strict production balance becomes:

```text
initial coarse storage
+ coarse precipitation
+ coarse climatology input
+ fine external additions
- fine external removals
- outlet exports
=
current coarse storage
+ current authoritative sparse GPU volume
```

Promotion/demotion are representation transfers and therefore do not appear as
external creation/loss terms.

The older `expect_no_untracked_fine_flux=true` request option remains only as a
controlled-test fallback for scenes that intentionally omit the production ledger.

## Sink-clipping GPU gate

Added:

```text
tests/water/FineExternalFluxLedgerGPU.tscn
tests/water/test_fine_external_flux_ledger_gpu.gd
```

The fixture starts one uniform 8x8 tile at 0.05 m depth and advances 0.5 s with:

```text
add rate    = 0.01 m/s
remove rate = 0.20 m/s
```

Each cell therefore receives 0.005 m but requests removal of 0.1 m. Only 0.055 m
is actually available, so the sink must clip there.

The gate verifies:

```text
reported gross add     == 0.005 * tile_area
reported gross removal == 0.055 * tile_area
reported net           == add - removal
final water volume     == 0
```

and explicitly rejects the larger requested sink volume as the ledger result.

Suggested local run:

```text
godot --path . tests/water/FineExternalFluxLedgerGPU.tscn
```

The current ChatGPT environment does not contain the project Godot 4.7 executable,
so this renderer/GPU gate is implemented but not runtime-passed here.
