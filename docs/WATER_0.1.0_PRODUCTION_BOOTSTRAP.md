# Water 0.1.0 — Production sparse hydrology bootstrap

Branch: `water/0.1.0`

## WaterSystem ownership

`WaterSystem` now owns the normal-world Phase-3 sparse hydrology lifecycle. The
legacy ocean coat/query path still starts independently; sparse hydrology is an
additional physical runtime and does not enable the dynamic visual contribution by
default.

```text
Planet.world_ready
      |
      v
HydroMetricGrid
  target dx -> exact quadtree level-compatible dx
      |
      v
SparseHydroScheduler
      |
      v
SparseHydroAtlasGPU
      |
      v
SparseHydroIdentityBridge
      |
      v
SparseHydroConnectivityGPU
      |
      v
HydroReachabilityService
      |
      v
SparseHydrologyRuntime
      |
      +--> HydroTerrainBedGPU
      +--> HydroSourceIngress
      +--> adaptive sparse SWE
      +--> activity/frontier/activation loop
```

## Bootstrap defaults

Current conservative defaults in `WaterSystem`:

```text
capacity            1024 slots
tile resolution     16 x 16 cells
target dx           4 m
macro dt            0.05 s
max time debt       0.50 s
max GPU substeps    16
```

The requested 4 m cell size is not used blindly. For each generated world's planet
radius, `HydroMetricGrid.contract_for_target()` selects the nearest discrete
cube-sphere quadtree level and `SparseHydroAtlasGPU` is initialized with that
level's **exact compatible cell size**. Terrain reconstruction, topology and SWE
metres therefore describe the same physical footprint.

At 1024 slots and 16x16 cells the core A/B/source atlas is roughly 12 MiB before
small occupancy, topology, activity and scheduler buffers, keeping the initial
production allocation reasonable for lower-end GPUs while leaving capacity a
runtime-tunable policy value.

## World lifecycle

`WaterSystem` listens to `Planet.world_ready`. A world rebuild can change:

- planet radius;
- global terrain texture;
- procedural terrain seed;
- terrain detail configuration.

Therefore a new `world_ready` invalidates and rebuilds the sparse GPU stack instead
of reusing world-specific RIDs/state. Stable external point-source definitions are
kept by `WaterSystem` and replayed into the new runtime.

## Renderer fallback

If `RenderingServer.get_rendering_device()` is unavailable (for example a true
headless or Compatibility path), sparse hydrology stays offline with state:

```text
unavailable_no_rendering_device
```

This is intentionally fail-soft. The existing ocean rendering/query services are
not disabled because the sparse Phase-3 runtime could not start.

## Public state

```gdscript
WaterSystem.sparse_runtime_available()
WaterSystem.sparse_runtime_state()
WaterSystem.sparse_metric_contract()
WaterSystem.sparse_runtime()
WaterSystem.gpu_stats()
```

Lifecycle signals:

```text
sparse_runtime_state_changed(state)
sparse_runtime_ready
sparse_runtime_failed(error, stage)
```

## Stable point-source API

Systems do not need to wait for `Planet.world_ready` before registering localized
water ingress/egress:

```gdscript
WaterSystem.upsert_point_water_source(
    "reservoir_outlet_17",
    surface_direction,
    12.0,                 # m3/s, signed
    injection_velocity,
)
```

Definitions are cached in `WaterSystem`. If the runtime is already ready the change
is forwarded immediately; otherwise it is replayed after bootstrap.

Available operations:

```gdscript
upsert_point_water_source(...)
remove_point_water_source(id)
set_point_water_source_enabled(id, enabled)
point_water_source_count()
```

This allows weather, rivers, structures and gameplay to own source definitions
without becoming coupled to atlas/RID lifetime.

## Structure barriers

`WaterSystem.set_hydrology_structure_crest_provider()` installs the production
structure-side boundary policy used by `HydroReachabilityService`.

The callback can raise terrain crest elevation for dams, levees, flood gates,
foundations, player-built walls and similar barriers. Terrain itself comes from
`GroundHeightStore.sample_height()`, which already includes `TerrainDeltas`.

## Rendering remains gated

Production sparse hydrology is now bootstrapped physically, but:

```text
u_dynamic_surface_enabled = 0
```

remains the default render state. The existing fixed-domain reconstruction gate
proved the texture/render path; sparse-atlas -> visible local-water reconstruction
is the next renderer integration rather than making the physical atlas itself a
render authority.

## Validation status

The current ChatGPT environment does not contain the project's Godot 4.7 renderer
executable. The bootstrap and GPU components are statically integrated but have not
been runtime-passed here.

Useful runtime diagnostics once a generated world is loaded:

```gdscript
print(WaterSystem.sparse_runtime_state())
print(WaterSystem.sparse_metric_contract())
print(WaterSystem.gpu_stats())
```

Expected successful progression:

```text
waiting_for_world
 -> initializing_atlas
 -> initializing_connectivity
 -> initializing_runtime
 -> ready
```

## Next integration

The physical runtime is now in the ordinary world lifecycle. The next major visual
integration is sparse-atlas reconstruction into the shared dynamic surface cache:

```text
resident sparse tiles near render anchor
      |
      v
sample/reconstruct h+bed and velocity
      |
      v
WaterSurfaceResources RGBA32F cache
      |
      v
toroidal ocean/local-water coat
```

This reconstruction must remain a view/cache operation. Sparse hydrology remains
the physical authority and must not become camera-dependent.
