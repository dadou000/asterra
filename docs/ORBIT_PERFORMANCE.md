# Orbit Studio performance and far-terrain behaviour

## Idle frame pacing

The Studio loop is vsync-bound, so without pacing an idle editor re-rendered the whole scene at the display
refresh rate (about 180 fps on a 180 Hz display): roughly one CPU core and ~20% of a 4090 for a static view.

Studio now renders at full rate only while it is *busy* and otherwise holds to a low rate that still wakes
immediately on input (`Window::WaitForActivity` uses `MsgWaitForMultipleObjectsEx`).

Busy means any of:

- keyboard, mouse, size or focus messages, or a held mouse button (`Window::ConsumeInputActivity`);
- any RPC/MCP request (`EditorSessionRpcHost::RequestCount`);
- far terrain patches still being built (`StudioViewportRenderer::HasPendingTerrainWork`).

After 1.5 s without any of these the loop paces to `ORBIT_IDLE_FPS` (default **30**). Set `ORBIT_IDLE_FPS=0` to
disable the throttle (the old behaviour).

Measured on Earth (per-process GPU counters, Orbit alone):

| State | GPU | CPU |
| --- | --- | --- |
| Unthrottled (`ORBIT_IDLE_FPS=0`) | 20.9% | 0.92 cores |
| Idle throttled (default) | 3.7% | 0.26 cores |

## Orbital terrain patches

The adaptive far-terrain hierarchy (`AdaptiveMacroGlobeRenderer`) used to sample the terrain source for one
33x33 patch per frame *on the render thread* (2-3.5 ms each), and capped the selection at 256 patches.

- Patches are now built on worker threads (up to 6 in flight) and uploaded on the render thread (up to 8 per
  call), so refinement is no longer tied to frame rate or render-thread time.
- The selection cap is 1024 patches and the resident cap 2560.
- Jobs hold shared ownership of the terrain source (`MacroGlobeFingerprint(shared_ptr, ...)`), so a world
  rebuild can never free it under a running job. Callers that only pass a reference fall back to synchronous
  builds.

## Crater and relief cue from orbit

Under a high sun a crater has no shading, so it disappears from orbit even when its geometry is present. The far
appearance now scales albedo by a sun-independent relief term: floors (vertex lower than its neighbours) and steep
walls are darkened, rims are brightened. Neighbours outside a patch are sampled from the terrain source so patch
edges match their interiors.

## Hot iteration

- Saving the touched engine sources while Orbit runs: falls under the central watcher's native/generation
  fallback (engine sources), as with any engine C++ change.
- Changing terrain recipe properties (for example crater count): direct resource refresh; far patches rebuild from
  the new source and the old ones keep rendering until replaced.
- A failed patch build leaves the previous resident patches drawn.

## CPU terrain work and worker threads

Terrain has a large CPU side next to the GPU near-field generator: the f64 analytic source, the seven physical-page
build stages (geology, drainage, processes, exposed surface, biome weights, surface material, scatter), orbital patch
builds, picking and sun-visibility marching.

- Worker threads are named, so debuggers, crash dumps and profilers (and `SetThreadDescription` readers) show what a
  thread is: `Orbit.Main`, `Orbit.TerrainPages.<n>`, `Orbit.Routes.<n>`, `Orbit.GlobePatch`, `Orbit.HotIteration`,
  `Orbit.HotReloadHost`, `Orbit.MapLayers`, `Orbit.ProjectScan`, `Orbit.UniformPlanet`. New pools must pass a name to
  `jobs::JobSystem` and new long-lived threads must call `core::SetCurrentThreadName`.
- Pools no longer each take every hardware thread. The physical-page pool uses half of them (`jobs::PoolWorkerCount`,
  at least 2) and the route planner uses 2, so on a 16-thread machine the terrain pool (8) + orbital patch builds (6) +
  the render thread fit without oversubscribing. `ORBIT_TERRAIN_WORKERS=<n>` overrides the terrain pool size.
- The text readout (Diagnostics > Text readout) and `view.text_diagnostics` report `cpu_terrain`: pool
  workers/running/queued jobs, page rebuild state and counts, and orbital patches building/resident.

Hot iteration: the pool size and thread names are read at process start, so changing them is an engine-source change
handled by the automatic generation handoff; `ORBIT_TERRAIN_WORKERS` needs a restart. The readout updates live.

## Terrain layers and LOD bias

Each viewport can choose which terrain layers it draws and how much detail it asks for (Diagnostics properties >
Terrain layers, or `view.terrain_layers_get/set`, MCP `orbit_view_terrain_layers_get/_set`). It is transient view
state: nothing enters terrain identity, persistence or generation, and changes apply on the next frame.

- **Near-field terrain**: the production clipmap. Off removes it (the viewport shows only what is left).
- **Orbital globe patches**: the adaptive displaced-globe patches seen from space.
- **Ocean**: sea-level water, on the terrain and on the orbital globe.
- **Surface effects**: volume surface-effect stamps applied to the terrain.
- **LOD bias (stops, -4..+4)**: +1 halves the pixel size the orbital patch selector targets, doubles its patch
  budget (1024 -> 2048, resident cap 2.5x that) and lets it refine up to two levels deeper; -1 does the opposite and
  is cheaper (budget down to 1/8). It also scales the representation quality policy, so the near-field to orbital
  handover keeps the richer representation longer (+) or hands over to the smooth globe sooner (-; around -2 a
  planet seen from orbit turns into the smooth globe and all terrain detail disappears). The pixel target alone does
  nothing when the selector is budget- or level-bound, which is the normal case from orbit, hence the budget scaling.
  Positive bias costs GPU memory and patch builds roughly in proportion to the budget (the resident cap doubles at
  +1). It does not change the near-field clipmap's own level count or spacing (those are fixed by the session's
  clipmap config), and it is not persisted.

The orbital patch selector state is shared per body, so two views with different biases on the same body share
hysteresis; the last view drawn wins.

## Altitude changes and the sky-view table

Changing altitude used to cost about 25 ms of CPU per frame in frame setup (whole loop about 35 ms, versus
about 3 ms and 8 ms when still or flying at constant altitude). The atmosphere's sky-view lookup table is keyed by
an exact hash of the observer's radius, and `BuildSkyView` rebuilds it on the CPU, single-threaded, whenever that
changes, so any climb or descent rebuilt it every frame.

- `BuildSkyView` builds its rows in parallel (every texel is independent).
- The renderer snaps the altitude used for the table to about 2% steps (`QuantizedSkyObserverRadius`), so it is
  rebuilt a few times per altitude decade instead of every frame. The sky stays within a fraction of a pixel of
  exact because the table varies smoothly with altitude.

Measured on Earth between 60 km and 3.5 km: vertical motion now matches standing still (frame setup about 3.3 ms,
whole loop about 8 ms); boosted climbs about 7 ms and 12 ms.

Hot iteration: engine C++ change, picked up by the generation handoff; nothing is cached across restarts.

## Multi-second freezes at fixed altitudes (clipmap tier changes)

Found with the micro-profiler ([ORBIT_PROFILER.md](ORBIT_PROFILER.md)): climbing from the ground to orbit and back
produced nine ~3 s main-thread stalls, every one inside `dxcompiler.dll` under
`GpuFieldGenerator::GpuFieldGenerator` (`StudioViewportRendererBase.cpp`, `ComposeBase`).

Cause: the adaptive coverage tier changes the clipmap config at a few altitudes, and `ComposeBase` treated any
clipmap change as "rebuild everything", including the `GpuFieldGenerator`, which recompiles its compute shader on
the frame thread. The generator depends only on the planet and its terrain source.

Fixes:

- A clipmap-only change now rebuilds just the `TerrainPreviewRenderer`; the generator is kept
  (`recreateGenerator` in `ComposeBase`).
- `DxcShaderCompiler` memoises results per process, keyed by stage, shader model, flags, entry point and source
  text, so rebuilding any renderer or pipeline from unchanged HLSL no longer recompiles. Edited sources hash
  differently and miss, so live shader editing is unaffected.

Verified with a 0 → orbit → 0 altitude sweep (933 navigate steps, 6,297 frames): no hitch after startup, where
before there were nine. What remains is a one-time cost on the first terrain build of a process (DXC plus about 8 s
in the NVIDIA driver compiling the field compute pipeline); a persisted `VkPipelineCache` would remove it.
