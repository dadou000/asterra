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

## Dynamic clipmap levels

The near-field terrain is a fixed ladder of 2:1 clipmap levels (65x65 samples each, level `j` has spacing `2^j` m):
20 levels, from 1 m to a half extent of about 16,800 km. Level `j` only ever has one spacing, so a level that stays
active keeps its resident samples whatever the camera does, and the old "shift the whole ladder coarser" coverage
tiers are no longer used (`StudioTerrainRuntimeConfig::adaptiveCoverage.enabled = false`; they forced every level to
resample at fixed altitudes).

What changes per frame is which contiguous range `[first, last]` of the ladder is drawn and generated
(`terrain_view::ClipmapPlanner`, run by `TerrainPreviewRenderer::Draw`):

- **Finest level, from screen error.** The planner finds the nearest visible ground (a grid of rays across the
  viewport against the ground sphere, plus the ground directly below) and the sample spacing one pixel asks for there:
  `pixelsPerVertex x pixel angle x distance`. Levels finer than that are not drawn. Straight down from 3 km the finest
  active level is 4 m, from 20 km it is 32 m. The ground below always counts, so turning the camera never needs levels
  that were just dropped.
- **Coarsest level, from visible ground.** The ground arc out to the farthest visible point (the horizon arc when the
  horizon is in view, at least 1.5x the height above ground) sets how far the outer level must reach. Straight down
  from 800 m is 6 levels reaching 1 km; a horizon view from 1 km needs 13 levels reaching 131 km.
- **Hysteresis.** A fine level is added the moment demand reaches it but only dropped once demand is 0.4 octaves past
  it; the coarse end grows at once and shrinks 0.4 octaves late. A camera sitting on a boundary does not flicker.
- **Changing the range is cheap.** The renderer keeps all 20 sample buffers. An inactive level is skipped (no draw, no
  sample generation, no pending dirty work); a level that becomes active is regenerated in full once, in the same frame
  it is first drawn. The finest active level has no inner hole. Level samples do not depend on the active set.
- **Ground height.** `ComposeBase` passes the analytic terrain elevation under the camera as a hint; the planner adds a
  300 m relief margin so mountains ahead are not assumed farther away than they can be.

Controls: the viewport Diagnostics "Terrain layers" section (Dynamic clipmap levels, Pixels per vertex) and
`view.terrain_layers_set` (`dynamic_clipmaps`, `clipmap_pixels_per_vertex`, default 3; lower keeps finer levels longer;
off activates the whole ladder). The result is `clipmap_plan` in `view.text_diagnostics` and the "Clipmap:" line of the
text HUD. Stats are only valid while the production terrain is drawn (below the orbital hand-off).

Seeing the active levels: in the viewport Diagnostics, **Active clipmap rings** outlines only the active levels (the
finest and coarsest of the plan brighter), and **Tint terrain by clipmap level** colours the surface per level so each
level and its hand-off are visible. Over RPC they are `clipmap_rings` and `clipmap_levels` in
`view.terrain_overlays_set`; `view.text_diagnostics` lists each active level (`clipmap_plan.levels`: level, spacing,
half extent) and the text HUD prints them on its "Clipmap:" line.

**Level cross-fade.** A plan change used to switch a level on or off in one frame. A level the plan adds now fades in
and one it drops fades out over `clipmap_fade_seconds` (default 0.4 s; the Diagnostics "Level fade (s)" slider; 0 gives
the old instant swap for comparison). Levels still fading stay in the layout (generated and drawn), so the active range
shown in the HUD can briefly be wider than the planner's. The fade is a per-pixel screen-door dither in the clipmap pixel
shader: the fading level keeps a pixel where an interleaved-gradient noise value is below its coverage, and the level
around it takes the remaining pixels, so the two always add up to one surface. While the finer level is mid-fade the
hole in the coarser level is not cut. It is dithered instead, and it becomes a hard cut again once the finer level is
fully drawn. The outermost level dissolves into the sky. The water pass is not dithered (flat at sea level, so the two
levels coincide). Content does not change during the fade: it is generated once when the level is added and the mesh is
not morphed in time, only dissolved. Freezing the clipmap lets a dissolve under way finish.

**EXPERIMENT: distance-banded levels.** Off by default (the fixed 2:1 ladder, its planner and the cross-fade above stay the
production path). With `experimental_distance_bands` on (Diagnostics checkbox, `view.terrain_layers_set`, MCP
`orbit_view_terrain_layers_set`) the clipmap is predefined by camera
distance instead of a ladder: `clipmap_band_edges_meters` (default 100, 500, 2000, 10000, 40000, 160000, 640000,
2.56e6, 1e7, 4e7) gives each level a band of distance from the camera, level 0 = 0-100 m, level 1 = 100-500 m, level 2 =
500-2000 m and so on; the last edge is the farthest distance drawn. A level is drawn at a pixel only where that pixel's
distance from the camera is in its band, and neighbours cross-fade per pixel (screen-door dither, same noise, so a pixel
is owned by exactly one level) across a zone of +-15% around each edge. Rings therefore resize continuously as the camera
moves: a camera 300 m above the ground has no ground within 100 m, so level 0 fades away, and level 1 shows only the
ground that is 100-500 m from it (a ring whose inner radius grows as the camera descends), with no popping and no holes
to cut. Each level is a 513x513 window sized to its outer edge x 1.3 (spacing = window /
512), nothing is morphed to a parent, and only levels whose band overlaps the nearest..farthest visible ground are
generated or drawn (`clipmap_plan.banded`, `.levels[].band_inner_meters/.band_outer_meters`). Cost and caveats: the sample spacing at a band's inner edge is (outer / inner) x 1.3 / 512 of the distance, which is
about 14 screen pixels per vertex for a x5 band at 1392 px / 70 degrees (the 2:1 ladder is about 9), so wide bands are
coarser; add edges to tighten. Measured on the Earth project: the sweep from 800 m to 12,000 km showed no hitches and
frames of 4.6 ms near the ground rising to about 9 ms from orbit (the ladder is 5-8 ms), and orbit views match the
ladder's. Levels re-centre
on their own lattice, so each regenerates in full whenever the camera crosses one of its cells. The "Active clipmap rings"
overlay is not drawn in this mode (use Tint terrain by clipmap level and Wireframe). LOD bias and pixels-per-vertex do
not apply. Distances are adjustable: the Diagnostics "Band distance scale" slider (`clipmap_band_scale`, 0.1-10, default 1)
multiplies every edge (2 = every band twice as far from the camera), and `clipmap_band_edges_meters` sets each edge. Both
rebuild the terrain renderer (the scale is applied in 1/8-octave steps so a drag does not rebuild every frame).

**Wireframe and freeze.** Diagnostics also has **Clipmap wireframe** (the terrain mesh as lines; the water surface is
hidden so the rings show) and **Freeze clipmaps** (`clipmap_wireframe`, `clipmap_freeze` in `view.terrain_overlays_set`,
MCP `orbit_view_terrain_overlays_set`). Frozen stops everything that follows the camera: the plan, the window position
of every level, residency and generated content. The camera keeps moving, so you can fly away and look at the active
rings, their hand-off and the planner's range from outside, and fly back to see where the camera left the window.
Unfreezing snaps the clipmap back to the camera and re-plans. Combine with **Tint terrain by clipmap level** for
coloured rings. The HUD prints "Clipmap debug: FROZEN" and `clipmap_plan.frozen` / `.wireframe` report the state. While
frozen only the terrain pass is re-projected: the sky, atmosphere and other passes still use the live camera, and the
terrain shading's view vector assumes the camera is at the frozen centre, so lighting is only approximate away from it.
Float32 positions lose precision far from the frozen window (about a metre at 10,000 km), which is fine for inspection.

Measured: dynamic and fixed renders are pixel-identical at the same pose, and the terrain pass costs about 0.13 ms of
GPU either way, so this is about structure and bounded work rather than speed: levels outside the view are no longer
generated or drawn, and nothing about the hand-off to the orbital globe changed.

**Hot iteration.** The planner lives in `engine/terrain_view`, the range handling in `TerrainPreviewRenderer`, and the
controls in Studio; saving any of them takes the automatic Studio-generation handoff. A new level range is applied on
the next `Draw`; nothing needs a restart. Not done (from the design this followed): per-vertex sampling footprints and replacing the orbital globe with
clipmaps. Sinking transitions were measured and skipped for now: forcing the finest level to jump two octaves at a
fixed pose changes under 1% of the pixels by more than a small threshold, and a one-octave change at the planner's own
boundary is smaller, so a time-based handoff would add shader work for no visible gain. Revisit it if a pop is seen
while flying; the debug overlays above show where handoffs fall.

## Full clipmap renderer (ground to orbit)

The production clipmap is now the only terrain representation, from the ground out to orbit (`full_clipmap`, default
on; `view.terrain_layers_set full_clipmap=false` restores the hand-off to the orbital globe patches). The orbital
globe is not drawn at any altitude in this mode, and the representation resolver (which would hand over to a smooth
globe or an impostor as the body shrinks on screen) is overridden to the production surface.

What made it work:

- **The ladder reaches the whole visible hemisphere.** 20 levels reach ~16,800 km of half extent; the planner picks the
  range each frame (see above), so from orbit it draws about 5 levels (for example levels 11-15 at 1,000 km, 13-17 at
  4,000 km). The clipmap lattice is azimuthal-equidistant, valid out to the antipode.
- **Coarse levels carry more samples.** Fine levels (spacing under 256 m) use a 257x257 grid and coarse levels a
  513x513 grid (`ClipmapConfig::coarseGridResolution` / `coarseMinSpacingMeters`, set in
  `StudioTerrainRuntimeConfig`). Clipmap rings are centred under the camera, so from orbit the mid-disc is far coarser
  than the screen needs; with 65x65 levels the land read as flat blobs. The few coarse levels spend the extra samples,
  the many fine levels near the ground do not. `ClipmapLevelGridResolution` / `ClipmapLevelHalfExtentMeters` give a
  level's resolution and reach; residency, buffers, vertex counts, layout and planner all use them per level.
- **No z-fighting with the globe.** Early captures of the clipmap from orbit showed a crisp tan/green mosaic over all
  land. It was the orbital globe still drawing underneath the clipmap and fighting it for depth (ocean hid it because
  both are blue). Full-clipmap mode now suppresses every globe draw site, not just the representation weight.
- **LOD bias** scales the planner's target spacing (+1 stop = twice the samples per pixel) instead of the globe's.

Measured (RTX 4090, 4K-class viewport): terrain GPU time 0.1-0.2 ms and a 5-8 ms frame at every altitude from the
ground to 12,000 km; the first terrain build after launch is unchanged apart from the larger buffers.

Known differences from the globe: from orbit the clipmap shows the same continents, coastlines, ice cap and craters
(the larger craters at 1,000 km), but small crater speckle and the sharpest coast detail at 4,000 km are softer than the
globe's uniform-pixel patches, because rings are coarse away from the nadir; at 60-300 km it is also slightly softer
than the old hybrid. A denser coarse grid (1025) or per-ring resolution tuning would narrow that further.
