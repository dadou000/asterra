# Orbit MCP / RPC Automation

Status: **Canonical automation contract**

Orbit Studio is fully drivable without a human at the keyboard. This follows
`ORBIT_UI_RULES.md` §13 and §22: every capability is one command/service that
the UI, plugins and agents all reach.

```text
MCP client ──> tools/mcp_server/orbit_editor_mcp_server.py ──> JSON-RPC 2.0
                                                              127.0.0.1:4320
                                                                   │
                                          Studio RPC dispatcher (engine/editor_rpc,
                                          apps/editor Main.cpp)  ──> commands/services
```

The Python adapter holds no engine policy. Every tool is a thin wrapper over one
RPC method, and `orbit_rpc_call(method, params_json)` reaches any method that
has no dedicated tool yet.

## Parity rule

> **Anything a user can do in Orbit Studio must be reachable over RPC and MCP.**

When a workflow needs a step that is only reachable through UI clicks:

1. Do not script the UI. Extract the operation the UI button runs into a
   callable method (see `ProjectAuthoringUi::CreateProject`), make the button
   call it, and register an RPC method that calls the same code.
2. Add a dedicated MCP tool in `orbit_editor_mcp_server.py`.
3. Document it in the coverage table below.

## Connecting

Start Studio (`Orbit.exe` / `OrbitStudio.exe <project>`). It listens on
`127.0.0.1:4320`. Register the adapter with your MCP client:

```text
claude mcp add orbit-studio -- python <repo>/tools/mcp_server/orbit_editor_mcp_server.py
```

`ORBIT_RPC_HOST`, `ORBIT_RPC_PORT`, `ORBIT_RPC_TIMEOUT` configure the MCP
adapter. Studio uses `ORBIT_RPC_PORT` too (default `4320`), so isolated local
smoke runs can use a private loopback port without connecting to another Studio.
The bridge opens one short connection per call, so a Studio relaunch never
leaves it holding a dead socket.

### Live crash smoke test

`examples/mcp-smoke/` is a small, complete project with a startup world and a
celestial system. The live smoke runner copies it into a unique directory under
`build/`, points `LOCALAPPDATA` at disposable run data, launches the built
unified Studio on a private RPC port, calls every tool advertised by the MCP
adapter, then probes every unique RPC method referenced by the adapter. A
normal JSON-RPC error (for example, a required argument was omitted) is an
expected response; a dropped request, timeout, or Studio exit fails the run.
All project and user-data mutations stay in the disposable copy.

Build Orbit first, then run:

```powershell
python tools/tests/mcp_live_smoke.py --exe Orbit.exe
```

This is an opt-in Windows/Vulkan smoke test, not part of the fast headless CTest
suite. It may exercise tool defaults that mutate the disposable project. On a
failure, the runner retains `studio.log`, the adapter log and the mutated
project under `build/mcp-live-smoke/` for diagnosis; `--keep-run` retains them
after a successful run as well.

### In-app feedback

Every mutating MCP/RPC command shows a toast in the lower-right corner of
Studio (`MCP: <method>`), and every failed command (mutating or not) shows a red
toast with the error. Read-only queries stay silent so polling does not flood
the screen. See `RecordRpcNotifications` in `apps/editor/src/Main.cpp` and
`EditorUi::PushNotification`.

## Documentation server (offline)

Separate from Studio automation: `tools/mcp_server/orbit_docs_mcp_server.py` serves the documentation tree
(`docs/ORBIT_DOCS.md`). It reads the repository's Markdown blocks directly, so it works without Studio running.

```text
claude mcp add orbit-docs -- python <repo>/tools/mcp_server/orbit_docs_mcp_server.py
```

| Tool | Purpose |
| --- | --- |
| `orbit_docs_root` | engine summary, sections and routing; start here |
| `orbit_docs_for_task(task)` | starting packet for a task: nodes, invariants, playbooks, verification, rules |
| `orbit_docs_get(path, section?, offset?, max_chars?)` | read one node (or one section of it) |
| `orbit_docs_children(path)` | list a node's children |
| `orbit_docs_search(query)` | ranked search (legacy documents are indexed per section) |
| `orbit_docs_for_target(target)` | invariants, dependants and rules for a file, module or symbol |
| `orbit_docs_check` | validate links, routes, sources and symbols |

## Projects: create, open, switch

| MCP tool | RPC method | Notes |
| --- | --- | --- |
| `orbit_project_info` | `project.info` | Open project id, name, root, startup world. |
| `orbit_studio_eco_mode(enabled?)` | `studio.eco_mode_get` / `studio.eco_mode_set` | Reads or toggles Eco pacing; an RPC request wakes the Studio loop immediately. |
| `orbit_project_create(root, name)` | `project.create` | Creates the project at `root` and switches to it. |
| `orbit_project_open(path)` | `project.open` | Directory or `Project.orbit.toml`. Saves the current project first. |
| `orbit_project_discover(roots?)` | `project.discover` | Scans Documents, Desktop, Downloads and Orbit's Projects folder (up to 4 levels, 4 s budget) for projects, newest first. Same scan as the Project Browser's Find Projects section. |
| `orbit_project_recent` | `project.recent` | MRU list with `available` and `error`. |

**Switching relaunches Studio.** A Studio process is bound to one project, so
`project.create` / `project.open` save state, start a fresh Studio on the
target, and the old process exits. The RPC port is briefly unavailable and the
new process answers on the same port. The MCP tools wait for this by default
(`wait=True`, polling `project.info` until the returned project `id` matches);
raw RPC callers must do the same. Failures (bad path, unwritable root) return an
error and leave the current project untouched.

These methods run the same `ProjectAuthoringUi` operations as the Project
Browser's *Create Project* / *Open Project* buttons, including recent-project
bookkeeping.

**Studio resumes where you left off.** The primary view's camera pose (observer,
surface frame, look angles, zoom) and the simulation time are saved once a
second and on exit to `<project>/.orbit/StudioView.ini`, and restored when the
project opens (`StudioViewContinuity`). This is automatic, so there is no tool
for it; to bookmark or replay a pose explicitly use the existing view-pose
RPC/MCP pair. Saving stays off until the restore was attempted (or 30 s passed
without the target body becoming current), so a fresh default camera never
overwrites the remembered one. `--terrain-ui-smoke` runs skip it to keep their
default view deterministic. Delete the file to reset.

## Worlds

| MCP tool | RPC method |
| --- | --- |
| `orbit_world_active` | `world.active` |
| `orbit_world_list` | `world.list` |
| `orbit_world_describe(path)` | `world.describe` |
| `orbit_world_create(path, display_name)` | `world.create` |
| `orbit_world_open(path)` | `world.open` |
| `orbit_world_close` | `world.close` |
| `orbit_world_set_startup(path)` | `world.set_startup` |
| `orbit_world_set_display_name(path, display_name)` | `world.set_display_name` |

`world.open` / `world.close` must be standalone requests (never in a batch).

## Panels and the Shading tab

| MCP tool | RPC method | Notes |
| --- | --- | --- |
| `orbit_panel_list` | `studio.panel_list` | Every tab with `open` and `visible`. |
| `orbit_panel_focus(title)` | `studio.panel_focus` | Opens a tab by title (case-insensitive) and brings it to the front. |
| `orbit_panel_close(title)` | `studio.panel_close` | Closes a tab; reopen with focus. |
| `orbit_viewport_navigate(delta_seconds, mouse_dx, mouse_dy, move_right, move_forward, move_up, boost)` | `viewport.navigate` | One camera navigation step, same as right-mouse look + WASD/QE. Terrain navigation when the active body has terrain, reference-sphere navigation otherwise (same movement, no terrain). |
| `orbit_viewport_focus_surface(u, v, view_id)` | `viewport.focus_surface` | Moves the camera to a low vantage point over the terrain under viewport position (u, v) in 0..1 (top-left origin), exactly like double-clicking the terrain. `focused: false` when the position misses terrain. |
| `orbit_view_terrain_overlays_get/_set(view_id, flags...)` | `view.terrain_overlays_get` / `view.terrain_overlays_set` | Terrain diagnostic overlays: dirty_page_bounds, build_states, physical_lod, clipmap_rings (outlines the active clipmap levels only), clipmap_levels (tints the terrain by clipmap level), clipmap_sample_health (colours each vertex by what is wrong with its GPU sample: red bad elevation, green bad morph target, blue bad slope, grey healthy; rows of one colour are a corrupted strip), clipmap_hole_view (culls nothing and colours each vertex by why it would be culled: red beyond the horizon, green inside a finer level's hole, blue inside it while that level fades in, cyan culled by distance bands, grey drawn normally), clipmap_projection_view (culls nothing and colours each vertex by where its clip position lands: red non-finite, green behind the camera, blue outside the near/far range, cyan off screen sideways, grey on screen; to find triangles the GPU clips away), clipmap_shading_view (colours each terrain pixel by which interpolated shading input is bad, as emission: yellow biome weights sum to zero, magenta non-finite position, red/green/blue terrain normal / body-fixed normal / surface direction, dark grey fine), clipmap_wireframe (terrain as wireframe), clipmap_freeze (freezes the clipmap so the camera can fly away and inspect it), cache_status, authored_constraints, biome_weights, process_masks, drainage_vectors. Same toggles as the viewport Diagnostics properties. |
| `orbit_view_terrain_layers_get/_set(view_id, production_surface?, full_clipmap?, macro_globe?, ocean?, surface_effects?, lod_bias_stops?, dynamic_clipmaps?, clipmap_pixels_per_vertex?, clipmap_fade_seconds?, experimental_distance_bands?, clipmap_band_edges_meters?, clipmap_band_scale?, clipmap_partial_updates?, physical_pages?, clouds?, cloud_resolution_scale?, cloud_godray_strength?, cloud_light_volume?, cloud_volume_debug_altitude?, cloud_temporal?, cloud_lab?)` | `view.terrain_layers_get` / `view.terrain_layers_set` | Which terrain layers a viewport draws (near-field clipmap terrain, orbital globe patches, ocean, surface effects) and its LOD bias in stops (clamped to [-4, 4]; +1 keeps richer representations longer and doubles orbital patch resolution). `full_clipmap` (default on) draws the production clipmap from the ground to orbit and never uses the orbital globe. `dynamic_clipmaps`, `clipmap_pixels_per_vertex` and `clipmap_fade_seconds` (how long a level dissolves in or out, default 0.4 s, 0 = pop) (and the EXPERIMENTAL `experimental_distance_bands` with its `clipmap_band_edges_meters` list and `clipmap_band_scale` multiplier: clipmap level k is drawn only where the camera's distance to the terrain is in its band, neighbours cross-fading) control the dynamic clipmap planner (see [ORBIT_PERFORMANCE.md](ORBIT_PERFORMANCE.md)). `cloud_resolution_scale` (0.25-1, default 0.5) is the cloud ray-march resolution relative to the viewport (1 = crisp, about four times the cost). `cloud_godray_strength` (0-2, default 1) scales the crepuscular rays: the cloud pass removes the direct in-scatter of air that sits in cloud shadow from what the atmosphere pass added (0 = off and skips the extra march; same slider as the Terrain layers "God-ray strength"). `bypass_cloud_shadow`, `bypass_proxy_sun_shadow` (the sun shadow cast by authored Visibility Proxies) and `bypass_proxy_surfaces` (drawing them as lit geometry), see [Proxy sun shadow](#proxy-sun-shadow-and-visible-proxies), `bypass_sky_cache` (the radiance cache's sky-only fill, see [Sky-only cache channel](#sky-only-radiance-cache-channel)), `bypass_indirect_lighting` (final gather + hybrid reflections), `bypass_hybrid_reflections` (only the reflections stage, to tell the two apart), `bypass_radiance_cache` (only the radiance-cache fallback of the final gather), `bypass_near_field_water` and `bypass_atmosphere` (also skips the clouds drawn after it) (default false) each skip one stage of the frame for this view, to bisect a rendering artefact by toggling them one at a time (same checkboxes as the Terrain layers "Bypass ..." rows). `anti_aliasing` (`off` | `fxaa` | `taa`, default `taa`; the Terrain layers "Anti-aliasing" combo): TAA jitters the camera by a sub-pixel rotation (8-sample Halton), reprojects its history from the camera pair and depth, clips it to the 3x3 neighbourhood and uses FXAA on frames with no usable history (first frame, teleport over 2 km, lens change, resize); everything runs on the HDR colour before exposure and tone mapping and only in the lit view, `mesh_shadow_softness` (0..32, default 1: multiplies the sun's angular size in the PCSS mesh sun shadow, 0 = hard shadows), `gi_intensity` (0..16, default pi = physically correct diffuse bounce; scales the final gather's indirect light), `bypass_sdf_gi` (default false; skips the final gather's world-space fallback so only screen-space rays contribute), `bypass_sdf_terrain` / `bypass_sdf_proxies` (default false; leave the terrain height patch / the Visibility Proxies out of the mesh distance field, to A/B what each adds to the fallback), `sdf_debug_view` (0 off, 1 shaded, 2 step-count heat map, 3 distance, 5 stored surface radiance, 4 split: sphere traces the merged mesh distance field from the camera and shows it, to check the field against the rasterised image), `taa_jitter_scale` (0..1, default 1; 0 turns the jitter off so TAA is a plain temporal filter), `gi_only_view` (default false) shows only the global illumination, the final gather plus the radiance-cache cascade fallback, with no direct sun, sky fill, emission or reflections (same checkbox as the Terrain layers "GI only view"; needs indirect lighting on), `indirect_coverage_view` (default false) replaces the final gather's contribution with its coverage: red confidence, green gathered brightness on a log scale, magenta where the gather returned nothing, i.e. the pixel gets no indirect light. `cloud_light_volume` (default on) keeps a camera-centred cache of the optical depth towards the sun (three toroidal cascades, 250 m, 2 km and 8 km cells, usable out to about 11, 92 and 368 km from the camera, refreshed a few voxels per frame): cloud lighting then sees shadows from other clouds beyond the 6.7 km of in-march sun steps (a storm shadowing cirrus at a low sun) and god rays cost one lookup per step; off marches everything per sample, for comparison. `cloud_volume_debug_altitude` (metres, 0-40000, default 0 = off; -1 shows the scene depth buffer as log view-space distance instead, magenta where no depth was written) draws a horizontal slice of the light volume at that altitude over the view as a heatmap of the optical depth towards the sun (blue clear, yellow, red, white opaque; magenta = the voxel is not ready; nothing outside the three cascades; darker for the coarser cascades), to see where cloud shadows are in the volume. `cloud_temporal` (default on) accumulates the march over frames (reprojected through the cloud shell and clamped to the current neighbourhood) so the half-resolution march averages its sampling jitter away; off shows the raw single-frame march. `cloud_lab` (object) is the cloud lab: it replaces the weather with ONE isolated cloud of a chosen type (`type`: 0.05 stratus, 0.2 stratocumulus, 0.32 nimbostratus, 0.5 cumulus, 0.72 congestus, 1.0 cumulonimbus) with `coverage`, `cirrus` (anvil / high cloud), `precipitation`, `radius_meters`, exaggerated vertical development (`height_scale`, 0.25-4), `distance_meters` ahead of the camera, `maturity` (life cycle: 0 towering cumulus, 0.3 growing cumulonimbus, 0.6 mature with anvil, 0.9 dissipating), `organisation` (0 single cell, 0.5 multicell of mixed ages, 1 organised), `density` (0.2-6), `cirrus_sheet` (0-1, default 0: a patchy thin-cirrus layer on the anti-sun side of the cell, where the storm's shadow falls, to see clouds shadowing cirrus), `seed`, an optional sun (`sun_override`, `sun_elevation_degrees`, `sun_azimuth_degrees`) and `place: true` to (re)place it ahead of the camera; `get` returns the stored values plus `place_serial`. `clouds` (default on) ray-marches the body's cloud layer in the viewport (see [ORBIT_PERFORMANCE.md](ORBIT_PERFORMANCE.md)). Same controls as the viewport Diagnostics "Terrain layers" section. |
| `orbit_view_text_diagnostics(view_id, cursor_u?, cursor_v?)` | `view.text_diagnostics` | Complete numeric and text diagnostic for a viewport: camera position/heading/pitch, distance from the planet core, height above datum (sea level), above terrain and above the water surface, plus for the point below the camera and (with cursor_u/cursor_v) under the cursor: latitude/longitude, terrain and coarse elevation, detail delta, water depth and surface, radius from core, slope, downhill bearing, climate and biome weights, physical page/LOD, terrain runtime revisions, and `cpu_terrain` (physical-page pool workers/running/queued, page rebuild counts and products, orbital patches building/resident), and `clipmap_plan` (which clipmap levels the planner keeps active, with each level's `grid_resolution`, `drawn_vertices` vs `expected_vertices` and `fully_drawn` (false = rows of that level are not submitted): first/last level, finest spacing, coarsest reach, nearest ground, the spacing the screen asks for, plan changes) and `clouds` (the target body's built cloud field: layer count, mean coverage and optical depth, time bucket, whether it is GPU-resident; absent without a cloud layer). `text` is exactly what the HUD shows. |
| `orbit_view_text_diagnostics_set(enabled, view_id)` | `view.text_diagnostics_set` | Shows or hides that HUD over the viewport (the "Text readout" checkbox in the viewport Diagnostics properties). |
| `orbit_renderdoc_status` | `renderdoc.status` | RenderDoc availability (Studio must be launched with `ORBIT_RENDERDOC=1` and RenderDoc installed), whether a capture is in progress, and the path of the last `.rdc`. |
| `orbit_renderdoc_capture` | `renderdoc.capture` | Captures the next presented frame; poll `renderdoc.status` until `capturing` is false and `last_capture_path` changes, then open the `.rdc` in RenderDoc. Errors if RenderDoc is unavailable. |
| `orbit_mesh_import(source, name?, parent_id?, position?, euler_degrees?, scale?)` | `mesh.import` (+ `object.create` / `property.set` when `parent_id` is given) | Imports a glTF/GLB (file, folder or .zip) into `Content/Models/<name>/`, validating it first, and optionally creates and places a Static Mesh under `parent_id`. `mesh.import` itself takes `source` (.glb/.gltf or a folder) and `name`, copies into the open project and returns `asset_path`, triangle/material/texture counts, bounds and warnings. See [ORBIT_STATIC_MESH.md](ORBIT_STATIC_MESH.md). |
| `orbit_mesh_status` | `mesh.status` | Imported Static Mesh (glTF/GLB) load state, counts, texture streaming progress, bounds and warnings; see [ORBIT_STATIC_MESH.md](ORBIT_STATIC_MESH.md). `bypass_mesh_surfaces` in `view.terrain_layers_set` hides meshes. |
| `orbit_profiler_status` | `profiler.status` | CPU micro-profiler: configuration, frame-time summary, last 120 frame times and recent hitch captures (file path, frame ms, sampled stack frames). See [ORBIT_PROFILER.md](ORBIT_PROFILER.md). |
| `orbit_profiler_configure(enabled?, hitch_threshold_ms?, stall_threshold_ms?, capture_window_ms?, max_hitch_files?)` | `profiler.configure` | Turns the profiler on/off and sets the hitch and stall thresholds and capture window. |
| `orbit_profiler_capture(window_ms?, path?)` | `profiler.capture` | Writes the last few seconds of every thread and CPU core as a Perfetto trace and returns its path. |
| `orbit_profiler_viewport_capture(action?, duration_ms?, resolution?, scenario?)` | `profiler.viewport_capture`, `profiler.viewport_capture_status` | Start/cancel/query a viewport-only capture with scenario `static`, `walk_1_94_mps`, `surface_200_kmh`, `flight_2000_mps_5000m`, or `ground_to_orbit_20s`. Moving scenarios restore the original camera on completion or cancel. Defaults are 4 seconds, 1440p and static; `window_ms` aliases `duration_ms`. |
| `orbit_profiler_panel_get` | `profiler.panel_get` | State of the Profiler panel (View menu > Profiler): paused/live, snapshot source, options including viewport-only capture duration, resolution and scenario defaults, visible range, selected slice. |
| `orbit_profiler_panel_set(paused?, window_ms?, grouping?, freeze_on_hitch?, min_slice_ms?, viewport_capture_duration_ms?, viewport_capture_resolution?, viewport_capture_scenario?, filter?, reset_view?, view_begin_ms?, view_span_ms?, zoom_frame?, select_thread?, select_time_ms?, clear_selection?, zoom_to_selection?, load_trace?)` | `profiler.panel_set` | Everything the panel's controls do, including shared viewport-only capture duration, resolution and scenario defaults, Pause / Resume live, history window, grouping, pause-on-hitch, slice filters and timeline navigation. |
| `orbit_profiler_snapshot(top_scopes?, slowest?)` | `profiler.snapshot` | Analysis of the visible range of the panel's snapshot: frame stats, lane busy time, heaviest scopes, heaviest GPU passes (`top_gpu_passes`), longest slices with thread/core/self time, stall stack samples. |
| `orbit_map_open(view_id?)` | `map.open` | Opens the flat planet map in a viewport (switches it to `flat_map` mode, the viewport mode selector's **Flat Map**). The map is an equirectangular image of the target planet (1024x512, north up, longitude -180 at the left) with a 30-degree graticule, stronger equator/prime-meridian lines and a red marker for the camera, drawn after the output transform so exposure and tone mapping never touch it. It is generated progressively on the main thread (a blue bar at the bottom shows progress), so poll `map.status` until `complete`. |
| (same viewport, `body_map` mode) | `view.mode_set` | In the globe (`body_map`) view the viewport also draws a 30-degree lat/long graticule (equator and prime meridian stronger) and a marker for the camera, and double-clicking the globe travels there like the flat map; `map.travel` with `u`/`v` picks on the globe too. The overlay and the picking use the same lat/long convention as the HUD. |
| `orbit_map_status(view_id?)` | `map.status` | Active `layer`, the available `layers`, `has_source`, `rows_generated` / `rows_total`, `complete`, and the camera `marker` as `latitude_degrees` / `longitude_degrees` (same convention as the viewport text HUD: latitude = asin(y) with +Y the spin pole, longitude = atan2(z, x); `null` when unknown). |
| `orbit_map_layer_set(layer, view_id?)` | `map.layer_set` | `layer`: `elevation`, `biomes`, `temperature`, `precipitation`, `water_depth` or `tectonics` (plate identity and boundary influence). All layers come from the same samples, so switching never re-samples the planet. Same as the layer combo shown next to the mode selector in Flat Map mode. |
| `orbit_map_travel(latitude_degrees?, longitude_degrees?, u?, v?, perspective?, view_id?)` | `map.travel` | Travels to a point of the planet like double-clicking the map: the terrain camera moves to a low vantage over it. Give latitude/longitude, or `u`/`v` (0..1, origin top-left) of a position in the viewport showing the map (fails on the letterbox bars). `perspective` (default true) then returns the viewport to perspective mode; false stays on the map, where the marker follows. |
| `orbit_view_mode_set(mode, view_id?)` / `orbit_view_debug_field_set(field?, view_id?)` | `view.mode_set` / `view.debug_field_set` | Viewport mode (perspective, body_map, debug, system, flat_map) and, in debug mode, which terrain field is shown (omit `field` to list names). Same operations as the viewport mode selector and Debug tab. |
| `orbit_workspace_get` / `orbit_workspace_set(mode)` | `studio.workspace_get` / `studio.workspace_set` | Build, Planet, Universe, Simulation, Shading, Planning, Plugins; same as the top workspace tabs. Scene and Celestial remain accepted aliases. |
| `orbit_bubble_open(object_id)` | `studio.bubble_open` | Opens the element parameter bubble shown in the active mode toolbar. |

The Shading tab (content tree with folders, live-compiled HLSL shaders and
shader materials, a preview on selectable shapes and lighting) is fully
scriptable through `shading.*`; see [ORBIT_SHADING.md](ORBIT_SHADING.md) for the
contract, the hot path (shader edits never restart Studio) and the complete
tool list.

## Simulation time (Simulate / Pause / Step)

The transport band along the bottom of Studio drives the one simulation clock
that moves planetary rotation, orbits, the sun and the atmosphere/weather
(clouds read the same time). Studio starts **paused**. The colored Play/Pause
control toggles it, `<< Step` / `Step >>` move by the chosen step (1 s ... 10
days, playing or paused), `Speed` sets simulation seconds per real second, and
the local-time scrubber follows the active body's solar clock. Planetary UTC is
the prime-meridian clock: noon occurs when the strongest direct radiative star
is over the prime meridian. Local time adds the primary viewport observer's
body-fixed longitude zone, with one zone per 15 degrees (24 equal zones).
Both UTC and Local are displayed separately. With no radiative star, the clock
falls back to authored rotation phase; with no surface observer, Local equals
UTC. Dragging or clicking the scrubber adjusts the shared
simulation clock; crossing midnight wraps the indicator while time continues
smoothly. The bar's sky-color gradient shows the current observer's approximate
night, twilight and daylight over that local day, using the dominant star's
declination and the body's authored atmosphere scattering. The `time.*` RPC
methods use that same clock and zone. A playing clock
keeps Studio out of idle frame pacing. Backed by
`SimulationControls` (`engine/studio_ui`), the same object the RPC methods call.

| MCP tool | RPC method | Notes |
| --- | --- | --- |
| `orbit_time_get` | `time.get` | `playing`, `rate`, `time_microseconds`, `time_text`, active-body `utc_time`, `utc_time_seconds`, `local_time`, `local_time_seconds`, `solar_time`, `local_day_seconds`, `local_time_zone_hours`, nullable `observer_longitude_degrees`, `step_seconds`. |
| `orbit_time_set(playing?, rate?, time_microseconds?, step_seconds?, local_time_seconds?)` | `time.set` | Play / Pause, speed (negative runs backwards), jump to absolute or current viewpoint zone time, step size. |
| `orbit_time_step(seconds?)` | `time.step` | The Step buttons; negative rewinds. |

## Issue reports

The **Reports** panel (View menu > Reports, or *Report issue* in the transport
band) keeps many problems from one session. Each report has a status
(`unresolved`, `pending`, `resolved`), any combination of scopes
(`performance`, `visual_quality`, `bug`, `crash`, `other`), free tags, a title,
description and resolution note.

Every report captures a **condition**: a JSON snapshot of the simulation
time/rate, project, world, workspace, selection, camera, exact camera pose,
target body, latitude/longitude and heights, view diagnostics text, surface
debug mode and frame-time statistics. A **persistent** problem has one
condition. A **transient** problem (one that comes and goes) has a *starting
condition* and, once it stops, an *ending condition*, each with its own capture
button; a non-transient report has no end condition and rejects one. *Go to this
situation* (`reports.restore`) pauses the clock at the captured time and rate,
selects the captured body and puts the camera back on the captured pose (the
camera finishes on its own once the body has loaded).

Every condition also carries a **screenshot** of the viewport taken at the same
moment (PNG, `<project>/Reports/Screenshots/R-0004-start.png` / `-end.png`). The
Reports panel shows it under the condition and `reports.get` returns its path, so
a problem can be looked at straight away, without launching a test. Retaking a
condition replaces its picture; deleting a report deletes its pictures.

Reports are saved to `<project>/Reports/reports.json` on every change (a crash
never loses one; a damaged file is left alone and reported in the log). They
live in the project, so they travel with it. Logic: `engine/studio_reports`
(model, persistence, tests) and `ReportsController` (`engine/studio_ui`).

| MCP tool | RPC method | Notes |
| --- | --- | --- |
| `orbit_reports_list(status?, scope?, transient?, text?)` | `reports.list` | Newest first, without snapshots. |
| `orbit_reports_get(id)` | `reports.get` | Full report with start/end conditions, each with its `screenshot` (relative to the reports folder) and `screenshot_path` (absolute). `id` is `7` or `"R-0007"`. |
| `orbit_reports_create(title?, description?, scopes?, tags?, transient?, status?, capture_start?, screenshot?)` | `reports.create` | Captures the starting condition and a screenshot unless `capture_start=false` / `screenshot=false`. |
| `orbit_reports_update(id, ...)` | `reports.update` | Status, scopes, tags, text; `transient=false` drops the end condition. |
| `orbit_reports_capture(id, which, screenshot?)` | `reports.capture` | `which` is `start` or `end` (transient reports only); retakes the screenshot unless `screenshot=false`. |
| `orbit_reports_open_screenshot(id, which)` | `reports.open_screenshot` | Shows a condition's screenshot file in the file browser (the panel's **Show file** button). |
| `orbit_reports_show(id)` | `reports.show` | Opens the Reports panel on a report (screenshot, conditions, notes). |
| `orbit_reports_restore(id, which)` | `reports.restore` | Recreate the situation: time, body and camera. |
| `orbit_reports_delete(id)` | `reports.delete` | Ids are never reused. |
| `orbit_reports_export(id?, status?, path?)` | `reports.export` | Markdown write-up, optionally to a file. |

## Implementation planning

The **Planning** workspace is a project-local implementation tree. Capture a
future idea as a floating bubble, drag it to arrange the canvas, and set its
status or schedule it after another bubble. Each schedule link is a directed
predecessor relationship; cycles are rejected. Deleting a bubble leaves its
successors as floating ideas. Data is saved to
`<project>/Planning/implementation-plan.json` after each change.

| MCP tool | RPC method | Notes |
| --- | --- | --- |
| `orbit_planning_list` | `planning.list` | Bubbles include id, title, description, status, normalized canvas position, and optional predecessor id. |
| `orbit_planning_create(title, description?)` | `planning.create` | Adds a floating idea. |
| `orbit_planning_update(id, title?, description?, status?, after?, clear_after?, x?, y?)` | `planning.update` | Status is `idea`, `ready`, `in_progress` or `done`; set `after` to schedule, or `clear_after=true` to float it. |
| `orbit_planning_delete(id)` | `planning.delete` | Deletes a bubble and clears links to it. |
| `orbit_viewport_pose_get(view_id?)` / `orbit_viewport_pose_set(pose_json, view_id?)` | `viewport.pose_get` / `viewport.pose_set` | Exact camera pose (observer, surface frame, look angles) of a view; what reports capture and restore. |

Hot iteration: the model and UI are ordinary `engine/` native code (generation
handoff on save); reports are on disk, so they survive the handoff.

## Camera zoom and high-resolution captures

The viewport toolbar has **Zoom x** (type a value, **1x** resets, and the mouse
wheel over the view zooms), **Screenshot** and **Ultra 16K**. Zoom is a
telephoto factor on the field of view (0.5 to 100) applied to the camera every
frame, so picking, atmosphere and clouds all agree on it. A report captures and
restores it with the camera pose.

**Screenshot** renders the viewport at the full window resolution (no panel
chrome) and saves a PNG in `<project>/Screenshots/` (a path ending in `.bmp` gives a BMP, which `viewport.screenshot` also honours). **Ultra 16K** makes a
15360 px (long side) image with the viewport's aspect (15360x8640 for 16:9).
A 16K frame does not fit in GPU memory as one render (about 5 GB per 33 Mpx), so
Ultra is tiled: the camera is turned onto a 5x5 grid of 4K tiles with a
narrower field of view, exposure and the simulation clock are held still, and
the tiles are reprojected into one image. Turning a pinhole camera about its
centre changes no perspective, so there are no seams or parallax; very wide
fields of view are a little softer toward the corners (zoom in for the sharpest
result). It takes about 15 seconds and about 1 GB of GPU memory more than the
viewport itself; Studio stays responsive and returns to its size and
simulation state afterwards. Any custom size above 8K is tiled the same way.

| MCP tool | RPC method | Notes |
| --- | --- | --- |
| `orbit_view_zoom_get(view_id?)` / `orbit_view_zoom_set(zoom, view_id?)` | `view.zoom_get` / `view.zoom_set` | Same as the Zoom control. |
| `orbit_view_camera_get(view_id?)` / `orbit_view_camera_set(fov_degrees?, focal_length_mm?, view_id?)` | `view.camera_get` / `view.camera_set` | The protected viewport camera's vertical lens controls. Set exactly one of FOV (degrees) or focal length (mm); focal length assumes a 24 mm sensor height. |
| `orbit_eye_get(view_id?)` / `orbit_eye_set(view_id?, highlight_protection?, glare_threshold_nits?, highlight_attack_seconds?, daylight_adaptation_nits?, max_boost_stops?, nits_per_scene_unit?, ...)` / `orbit_eye_reset(view_id?)` | `display.eye_get` / `display.eye_set` / `display.eye_reset` | The Display Diagnostics eye-adaptation controls (see "Eye adaptation" below): read config and live state in cd/m2, change any subset of fields (validated; not persisted to the display settings file), or restart adaptation. |
| `orbit_viewport_capture_start(kind, width?, height?, path?, settle_frames?)` | `viewport.capture_start` | `kind`: `fullscreen`, `ultra`, `custom`. Returns at once. |
| `orbit_viewport_screenshots_open` | `viewport.screenshots_open` | Opens `<project>/Screenshots` in the file browser (the viewport **Screenshot Files** button). |
| `orbit_viewport_capture_status` | `viewport.capture_status` | Poll until `state` is `idle`; `last` has the file path, size or error. |

## Debug tab

One toolbar switching which GBuffer channel every Studio viewport ("Viewport"
and "Body Map / Debug View") renders. The same operation is reachable three
ways: the Debug tab's own toolbar, each Viewport panel's "Surface View"
button row, and `view.surface_debug_*` below — all three drive the same
`StudioRenderViewSet` setter (ORBIT_UI_RULES.md section 13).

| MCP tool | RPC method | Notes |
| --- | --- | --- |
| `orbit_view_surface_debug_get(view_id)` | `view.surface_debug_get` | `view_id` defaults to `"studio.primary"`; the other slot is `"studio.map"`. |
| `orbit_view_surface_debug_set(surface_debug_mode, view_id)` | `view.surface_debug_set` | `surface_debug_mode`: `lit` \| `base_color_roughness` \| `normal_metallic` \| `emission_metadata`. |

## Objects, bodies and properties

| MCP tool | RPC method |
| --- | --- |
| `orbit_object_roots` / `_children` / `_get` | `object.roots` / `.children` / `.get` |
| `orbit_object_create(type_id, name, parent_id)` | `object.create` |
| `orbit_object_rename` / `_reparent` | `object.rename` / `object.reparent` |
| `orbit_object_delete` / `_duplicate` | `object.delete` / `object.duplicate` (leaf objects only; undoable) |
| `orbit_property_set(object_id, property_id, value)` | `property.set` |
| `orbit_body_list` | `body.list` |
| `orbit_body_create(parent_id, name)` | `body.create` |
| `orbit_body_capabilities(body_id)` | `body.capabilities` |
| `orbit_body_set_capability(body_id, capability, enabled)` | `body.set_capability` |
| `orbit_celestial_create_from_recipe(kind, parent_id, ...)` | `celestial.create_from_recipe` | Star / rocky planet / moon from the physical recipes with explicit parameters (defaults are Sun / Earth / Moon). Same recipe service as the Celestial panel's Generate Seeded System. |
| `orbit_world_ensure_planet_surfaces` | `world.ensure_planet_surfaces` | Gives every spherical planet/moon without one a Terrain Surface (one undo step). New planets get one automatically; stars, giants, compact objects and ellipsoid bodies are skipped. |
| `orbit_celestial_capabilities(body_id)` / `orbit_celestial_set_capability(body_id, type_id, enabled)` | `celestial.capabilities` / `celestial.set_capability` (Celestial toolbar toggles; disabling keeps authored values, enabling a missing capability creates it) |
| `orbit_atmosphere_presets()` / `orbit_atmosphere_solve(atmosphere_id)` / `orbit_atmosphere_apply_preset(atmosphere_id, preset)` | `atmosphere.presets` / `atmosphere.solve` / `atmosphere.apply_preset` (Celestial panel preset buttons and Solve; `atmosphere_id` is the atmosphere capability object, not the body; returns the solver report `events[]` with `derived` / `no_change` / `conflict` / `invalid_input`, `derived_count`, `has_conflict`, `has_invalid_input`; locked or explicit properties come back as conflicts, an unknown preset or a non-atmosphere object is a `-32602` error) |
| `orbit_terrain_cache_stats(terrain_id, viewport="studio.primary")` | `terrain.cache_stats` (read-only; the persistent GPU cache statistics the Surface authoring panel and the cache overlay show: `cache{hits, misses, generations, insertions, evictions, resident_pages, resident_bytes, hit_rate_percent}`, `stationary{frames, cache_hits, cache_misses, hit_rate_percent}`, `physical_lod`, revisions; `terrain_id` is the terrain surface object; error 1004 when no world is open or the object has no terrain services; registered by `StudioSession` in `engine/studio_session/src/StudioTerrainStatusRpc.cpp`) |
| `orbit_terrain_tectonics_get(terrain_id)` | `terrain.tectonics_get` (read-only; persisted procedural plate recipe used by the Planet toolbar and analytic terrain generator) |
| `orbit_terrain_tectonics_sample(latitude_degrees?, longitude_degrees?, viewport="studio.primary")` | `terrain.tectonics_sample` (read-only planet structural layer at a point, or under the viewport observer when coordinates are omitted: `plate_id`, `neighbour_plate_id`, plate types, `boundary_type` (`none`/`convergent`/`divergent`/`transform`), `boundary_strength`, `convergence`/`divergence`/`transform` influences, `crust_thickness_km`, `crust_age` and `geological_age` (0 new, 1 ancient), `uplift_meters`, `subsidence_meters`, `stress`, `volcanism`. Derived analytically from the persisted tectonics recipe; same data as the Tectonics toolbar menu readout) |
| `orbit_terrain_erosion_coupling_get(terrain_id)` | `terrain.erosion_coupling_get` (read-only; `age_erodibility_gain`, `age_uplift_decay`, `tectonic_drainage_guidance` persisted on the terrain process settings) |
| `orbit_terrain_erosion_coupling_set(terrain_id, settings)` | `terrain.erosion_coupling_set` (partial update, one undoable `SurfaceAuthoringModel.SetProcessSettings` edit plus global process-settings invalidation, same operation as the Planet Tectonics menu's Geological Coupling section. `age_erodibility_gain` 0-8 scales stream-power erodibility by `1 + gain * geologicalAge`; `age_uplift_decay` 0-1 scales tectonic uplift by `1 - decay * geologicalAge`; `tectonic_drainage_guidance` 0-1 steers M09 routing from uplifting belts toward basins and rifts without ever routing uphill) |
| `orbit_terrain_bake_status(terrain_id)` | `terrain.bake_status` (read-only; planet bake `state` none/baking/ready/stale/failed, `progress`, `resolution`, `auto_rebake`, `active_resolution`, `active_bytes`, `rivers_active`, `river_nodes`, `river_segments`, `river_bytes`, `current_river_hash`, `active_river_hash`, `current_recipe_hash`, `active_recipe_hash`, `last_bake_seconds`, `error`, `path`. Terrain generation samples the bake and never evaluates the plate model) |
| `orbit_terrain_bake_start(terrain_id, resolution?)` | `terrain.bake_start` (background bake now, 16-2048 texels per cube-face edge; the running bake keeps driving terrain until the new one validates and swaps in) |
| `orbit_terrain_bake_cancel(terrain_id)` | `terrain.bake_cancel` (stops a running bake; the active bake is untouched and no automatic rebake starts until the recipe changes or a bake is started) |
| `orbit_terrain_bake_set(terrain_id, resolution?, auto_rebake?)` | `terrain.bake_set` (persisted bake policy, one undoable `SurfaceAuthoringModel.SetProcessSettings` edit, same operation as the Planet Tectonics menu's Planet Bake section) |
| `orbit_terrain_tectonics_set(terrain_id, settings)` | `terrain.tectonics_set` (partial recipe update; one undoable edit through `SurfaceAuthoringModel`, same operation as Planet toolbar; settings keys are `seed`, `plate_count`, `plate_irregularity`, `plate_size_variance`, `continental_fraction`, `continental_crust_bias_meters`, `oceanic_crust_bias_meters`, `continent_influence`, `boundary_width`, `minimum_angular_speed`, `maximum_angular_speed`, `convergence_reference_speed`, `transform_reference_speed`, `convergent_uplift_meters`, `oceanic_collision_scale`, `hotspot_count`, `hotspot_age_steps`, `hotspot_relief_meters`, `hotspot_age_decay`, `hotspot_chain_spacing_meters`, `hotspot_core_radius_meters`, `hotspot_radius_growth_per_age`; omitted fields retain their values) |
| `orbit_terrain_hydrology_get(terrain_id)` / `orbit_terrain_hydrology_set(terrain_id, settings)` | `terrain.hydrology_get` / `terrain.hydrology_set` (persisted rainfall, infiltration, soil moisture, evaporation and seasonal amplitude/period/phase; partial update, one undoable `SurfaceAuthoringModel` edit, then global process-settings invalidation; static terrain precipitation field is sinusoidally scaled by simulation time in 12 bins per cycle) |
| `orbit_terrain_drainage_spline_add(terrain_id, points, half_width_meters, falloff_meters, guidance=1.0)` | `terrain.drainage_spline_add` (creates an undoable Drainage-channel terrain spline from unit-direction points; bounded M27 invalidation; guidance biases only downhill routing; selects the new constraint) |
| `orbit_terrain_rivers_get(terrain_id)` | `terrain.rivers_get` (read-only persisted mountain-fed river recipe used by the Planet toolbar and M09/M16 terrain generation) |
| `orbit_terrain_rivers_nearby(terrain_id, viewport="studio.primary")` | `terrain.rivers_nearby` (read-only river nodes, segments and boundary links from built pages in the observer neighborhood (the baked river graph clipped to each page, or the local M16 solve for pages with authored constraints). Nodes expose channel dimensions, water level, slope, Manning velocity and roughness, cross-section area and suspended-sediment inventory; segments expose routing points and aggregate hydraulic readouts; boundary links expose upstream node, basin, page, flow direction and target edge cell. Pending pages are reported without synthesizing graph data) |
| `orbit_terrain_lakes_nearby(terrain_id, viewport="studio.primary")` | `terrain.lakes_nearby` (read-only M09-derived lake basins on already-built nearby pages; reports surface, depth, area, stable page-local ID and spill/outlet cells, plus the first downstream M16 river node when one exists on that page; pending pages and page-exit spills are explicit) |
| `orbit_terrain_river_constraint_add(terrain_id, basin_id, kind, page_face, page_level, page_x, page_y, center_meters, radius_meters=500, strength=1, direction_meters?)` | `terrain.river_constraint_add` (persists an attract, repel, or trajectory intent for one basin/page in page-local meters, then queues bounded terrain regeneration; values from `orbit_terrain_rivers_nearby` identify the basin/page/center) |
| `orbit_terrain_rivers_set(terrain_id, settings)` | `terrain.rivers_set` (partial recipe update; one undoable edit through `SurfaceAuthoringModel`, same operation as Planet toolbar; keys cover `enabled`, `maximum_node_spacing_meters`, drainage/discharge thresholds, base/min/max channel width and depth, discharge exponents, meander enablement/iterations/time step/migration rates/offset, and cutoff enablement/path length/distance; omitted fields retain values) |
| `orbit_object_transform_info(object_id)` / `orbit_object_transform(object_id, tool, axis, amount, space="world")` | `object.transform_info` / `object.transform` (the Move / Rotate / Scale gizmo operation without the pointer: `tool` translate = meters, rotate = degrees, scale = positive factor; `axis` x / y / z / uniform (scale only); `space` world or local, scale always local; one undoable step per call; supported types Primitive, Visibility Proxy, Static Mesh (move, rotate, uniform scale), Point Light (move), Spot Light (move, rotate); other types and bad arguments are `-32602`, a missing object `1004`; implemented by `editor_model::ViewportManipulator`) |
| `orbit_schema_catalog` / `orbit_command_catalog` | `schema.catalog` / `command.catalog` |
| `orbit_transaction_begin/commit/rollback`, `orbit_undo`, `orbit_redo` | `transaction.*`, `history.*` |
| `orbit_selection_get/set/clear` | `selection.*` |
| `orbit_viewport_get/set_camera/screenshot/focus_body` | `viewport.*` |
| `orbit_viewport_snapping_get` / `orbit_viewport_snapping_set(translation_enabled?, distance?, unit?, surface_enabled?, rotation_enabled?, rotation_degrees?, scale_enabled?, scale_percent?)` | `viewport.snapping_get` / `viewport.snapping_set` | Shared toolbar/Inspector snapping. Distance defaults to meters; mm, cm, m, km, in, ft supported. Unit-only updates preserve physical distance. Angles use degrees; scale uses percent. Positive finite steps are validated before any change, and settings persist with Studio shell state. |
| `orbit_viewport_frame_selected` | `viewport.frame_selected` | Frames the selected object in the controlled perspective viewport, like the Scene toolbar and F shortcut. Visibility-proxy hierarchies use their aggregate bounds; a selected body uses full-body focus. |

Type and property IDs are stable GUIDs; `schema.catalog` lists all of them with
kind, unit, default, `advanced` and `read_only`. Use `transaction.begin` /
`commit` to make a multi-property edit one undo step.

## Recipe: a single Moon-equivalent planet in a new project

Empty worlds have no objects, so the hierarchy is World -> Celestial System ->
Celestial Body. Every call below is available as an MCP tool.

```text
project.create   {"root": "<repo>/OrbitTestProject", "name": "Moon Test"}
                 (wait for the relaunch; orbit_project_create does this)

object.create    {"type": "4f524249-5457-4f52-4c44-545950450001", "name": "Moon World"}      -> worldId
object.create    {"type": "4f524249-5453-5953-5445-4d5459500001", "name": "Moon System",
                  "parent": worldId}                                                          -> systemId
body.create      {"parent": systemId, "name": "Moon"}                                         -> bodyId

transaction.begin {"label": "Author Moon body"}
property.set  object=bodyId, then one call per row:
transaction.commit
```

| Property | ID | Value | Moon reference |
| --- | --- | --- | --- |
| Equatorial / Reference Radius (m) | `4f524249-5450-524f-5052-414449555301` | `1737400.0` | mean radius 1737.4 km |
| Polar Radius (m) | `4f524249-5450-524f-5050-4f4c41520001` | `1737400.0` | sphere (flattening ~0.001) |
| Mass (kg) | `4f524249-5450-524f-504d-415353000001` | `7.342e22` | 7.342e22 kg |
| Rotation Period (s) | `4f524249-5450-524f-5052-4f5450455201` | `2360591.5` | 27.321661 d sidereal |
| Axial Tilt (deg) | `4f524249-5450-524f-5054-494c54444501` | `6.68` | 6.68 deg to orbit plane |

Ellipsoid Shape (`4f524249-5450-524f-5045-4c4c49505301`) stays `false`.

### Add the light source

A lone body has no star, so the viewport is black. Author a Sun in the same
system: a second body plus Photosphere and Radiative Emitter capability objects
(their defaults are already solar), placed 1 AU from the Moon.

```text
body.create      {"parent": systemId, "name": "Sun"}                                          -> sunId
property.set     object=sunId, one call per row (inside one transaction):
                   Equatorial radius  4f524249-5450-524f-5052-414449555301 = 6.957e8
                   Polar radius       4f524249-5450-524f-5050-4f4c41520001 = 6.957e8
                   Mass               4f524249-5450-524f-504d-415353000001 = 1.989e30
                   Parent-frame Pos.  4f524249-5450-524f-5050-4152454e5401 = [1.495978707e11, 0, 0]
object.create    {"type": "4f524249-5443-454c-5048-4f4341500001", "name": "Sun Photosphere",
                  "parent": sunId}
object.create    {"type": "4f524249-5443-454c-454d-544341500001", "name": "Sun Radiative Emitter",
                  "parent": sunId}
```

The perspective viewport now includes other bodies in the active celestial
system at their simulated positions and apparent sizes. A `viewport.screenshot`
can capture the Sun and Moon without changing the active body. Use
`selection.set` to focus the Moon when inspecting its own surface.

### Add regolith and craters

The engine's regolith shading is the **Small Body Appearance** capability
(`4f524249-5443-454c-534d-414c4c415001`, model `Procedural Regolith`), attached
as a child of the body with `object.create`. Property IDs share the prefix
`4f524249-5453-4d42-`; the values below give a lunar look:

| Property | ID suffix | Value | Why |
| --- | --- | --- | --- |
| Model (`4f524249-5443-454c-5052-4f504d4f0001`) | | `"Procedural Regolith"` | the only supported model |
| Body Class | `434c-415353000001` | `"Moonlet"` | Asteroid, Comet Nucleus or Moonlet |
| Seed | `5345-454400000001` | `1969` | crater layout |
| Axis Scale | `4158-495353434c01` | `[1,1,1]` | a sphere, not a lumpy asteroid |
| Irregularity / Large Lobe | `4952-524547554c01` / `4c4f-424553545201` | `0.004` / `0.0` | near-flat relief |
| Regolith Roughness | `5355-524652474801` | `0.95` | |
| Regolith Color | `5245-47434f4c0001` | `[0.11,0.105,0.10]` | dark, albedo about 0.12 |
| Fresh Material Color | `4652-45434f4c0001` | `[0.20,0.19,0.175]` | about 1.8x brighter ejecta |
| Color Variation | `434f-4c5641520001` | `0.30` | |
| Crater Density | `4352-44454e530001` | `1.0` | 24 craters is the engine maximum |
| Crater Depth | `4352-444550540001` | `0.03` | fraction of body radius (see below) |
| Crater Rim Strength | `4352-52494d530001` | `0.015` | |
| Single-scattering Albedo | `5353-43414c420001` | `0.2` | |
| Opposition Strength / Width | `4f50-505354520001` / `4f50-505749440001` | `0.9` / `0.05` | lunar opposition surge |
| Macroscopic Roughness | `4d41-43524f550001` | `0.35` | about 20 deg, a Hapke angle in radians |

Caveats found while tuning:

- **Crater Depth is a fraction of the body radius** and displaces the surface.
  Real lunar craters are about 0.2-0.3% of the radius deep; the schema default
  scale (0.12) looks lumpy on a Moon-sized body.
- **Macroscopic Roughness is an angle, not PBR roughness.** It used to be packed
  into the far-body G-buffer's roughness channel (`DrawSurfaceData` in
  `engine/celestial_far_render/src/FarBodyRenderer.cpp`), so 0.35 became GGX
  roughness and painted a glossy sun-mirror hotspot on the disc. The G-buffer
  now receives the regolith roughness, and the physical value is safe to use.
- **Impostor lighting frame.** The impostor path (`mode == 1`) built its view
  vector from the mirrored surface normal and used a screen-space normal with a
  body-fixed light. It now rotates the normal into the body frame with the
  camera basis and looks along the camera axis, matching the ray-traced path.
- The far-disc shader draws at most 24 large craters (60-250 km radius); small
  craters need the near-field surface path.

Notes:

- `viewport.focus_body` returned `focused: false` here even though the Moon was
  selected and rendered; the default camera already frames it. Use
  `viewport.set_camera` to move it (terrain navigation overwrites it on the
  next frame; see the Studio camera notes).
- `object.create` also attaches other capabilities the same way, e.g. Orbit /
  Ephemeris (`4f524249-5443-454c-4f52-424341500001`) to put the Moon in orbit.

## Proxy sun shadow and visible proxies

Authored Visibility Proxies (`object.create` with type
`4f524249-5456-4953-5052-4f5859000001`, shape box/sphere, body-local position
and Euler rotation) are invisible lighting occluders. Besides the radiance cache
and exact reflections they now shadow the **direct sun**: a compute pass
(`ProxySunShadowRenderer`, `engine/lighting`) traces one hardware ray per visible
pixel toward the star through the same acceleration structure the exact
reflections use, writes a full-resolution visibility texture and
`DirectLightingRenderer` multiplies it into the stellar term next to the cloud
shadow. Terrain and sky are not proxies, so only authored structures cast.

- It runs only when the device supports ray queries and the target body has at
  least one proxy; otherwise direct lighting is unchanged.
- **Visible proxies.** `ProxySurfaceRenderer` also rasterises every proxy as lit,
  depth-tested geometry (boxes by triangles, spheres ray-traced in the pixel
  shader) into the deferred surface buffer right after the terrain pass, as
  `RigidGeometry` / `LocalMesh` surfaces: neutral grey for Material ID 0, a
  stable tint per other Material ID, roughness 0.85. They therefore receive the
  direct sun, the proxy sun shadow, the screen-space gather and the radiance
  cache like any surface, and the gather can bounce light off them. This needs
  no ray-query support. `bypass_proxy_surfaces=true` (UI checkbox "Bypass proxy
  surfaces", `view.terrain_layers_set`) returns to invisible occluders.
- Toggle it with `orbit_view_terrain_layers_set(bypass_proxy_sun_shadow=true)`
  (RPC `view.terrain_layers_set`, the Terrain layers "Bypass proxy sun shadow"
  checkbox); `orbit_view_terrain_layers_get` reports `bypass_proxy_sun_shadow`.
- Hot iteration: proxy property edits take the existing semantic-revision
  rebuild (no restart; the primitive buffers for both passes are rebuilt with it). Saving `ProxySunShadow.cpp` or `DirectLighting.cpp`
  takes the automatic Studio-generation handoff; the embedded HLSL compiles at
  startup, so a failed shader leaves the running generation alive.
- **Sky fill on proxy surfaces.** The same compute pass also traces 12
  cosine-weighted rays per proxy-surface pixel against the other proxies (150 m)
  and writes the atmosphere's sky irradiance times the open fraction; direct
  lighting adds it as albedo / pi fill on proxy surfaces only. Terrain keeps its
  existing lighting, so a closed room is dark inside except for what the
  openings let in (sun shafts, and sky through the openings), and a roof's
  underside or an enclosed floor is not lit by the open sky. `bypass_proxy_sun_shadow`
  turns off the sun shadow and this fill together.
- **Proxy surfaces ignore the radiance cache.** They are lit exactly (ray-traced
  sun and sky fill), so the cache fallback of the final gather and the hybrid
  reflections' cache fallback skip pixels of surface class 3. A cache cell is a
  cube aligned to the planet frame that straddles thin walls and roofs: its
  lifted sample point can sit on the other side of the wall, which lit inner
  walls and ceilings with outside light and, because cells fill in a few at a
  time and the finest valid level wins, made the wall pop between light and dark
  in cell-shaped triangles one cell after another. Terrain keeps using the cache.
- **GPU origin refresh.** The proxy scenes (acceleration structure and primitive
  buffers) are float32 relative to the GPU origin they were built at. A scene
  built while the camera was far away (a world reopened from a planet-scale
  view) resolves only about a metre, which showed up as bands of wrongly
  occluded rays on proxy walls. The renderer now rebuilds with a fresh origin
  once the camera is more than 1.5 km from it while within 20 km of the
  proxies (`ProxyGpuOriginIsStale`); farther away they are sub-pixel and are
  left alone. The exact reflections share the same scene and benefit too.
- Proxy shadows still do not darken terrain's own sky or bounce light: the
  radiance cache traces proxies for those, but only for pixels the screen-space
  gather leaves unresolved.

## Sky-only radiance cache channel

Each radiance cache cell now carries a **sky-only** irradiance next to its
one-bounce L1 (cell grew from 64 to 96 bytes: `skyIrradiance.rgb` = L0,
`skyGradient.xyz` = direction toward the open sky, `skyGradient.w` = the
one-bounce transport the L1 used). The estimator
(`EstimateRadianceCellWithSky`) fills it with the atmosphere's sky irradiance
times the open fraction of the cell's hemisphere, traced against everything the
visibility registry knows: **terrain** (ridges and valleys), **authored
proxies**, and analytic bodies. Unlike the old L1 sky it is not scaled by the
0.18 one-bounce transport.

`DirectLightingRenderer` reads the cache and adds the sky as `albedo / pi *
E(n)` fill, with `E(n) = l0 * (1 + dot(gradient, n))`, on every near-field
surface, independent of how confident the screen-space gather is. This is what
lights cast shadows and enclosed spaces from the sky instead of leaving them
black. It is added to scene colour after the gather has read it, never into the
gather's history, so it cannot accumulate frame to frame. Authored proxy
surfaces keep their ray-traced sky fill instead (more accurate than a cell).

- Reflections are unchanged: the hybrid and exact reflection shaders rebuild the
  legacy one-bounce sky from the new channel (`2 * l0 * transport`, old lobe
  weights), and the cache fallback no longer carries a sky (the fill replaces it).
- Toggle: `orbit_view_terrain_layers_set(bypass_sky_cache=true)` (RPC
  `view.terrain_layers_set`, the Terrain layers "Bypass sky cache fill"
  checkbox); `get` reports `bypass_sky_cache`. It only changes the fill; the
  cache keeps estimating the channel.
- Hot iteration: the estimator, cell layout and shaders are lighting-library
  sources (automatic Studio-generation handoff). The change needs no content
  migration: a new generation starts with an empty cache that refills.
- Night side and space: the atmosphere's sky summary is zero without sunlight,
  so the fill vanishes; it is also off whenever the near-field indirect stack is
  (planet seen from orbit, `bypass_indirect_lighting`).

## Eye adaptation: highlight protection and boost limit

Auto-exposure (`engine/post_process/HumanEyeAdaptation`) works in cd/m^2. One scene-linear unit is
1361 W/(m^2 sr) x 683 lm/W = 929,563 cd/m^2 (`kSceneLuminanceNitsPerUnit`, `nitsPerSceneUnit`).

- **Highlight protection** (`display.eye.highlight_protection`, on by default): the brightest metered
  pixel never exceeds the display peak. Exposure is capped so that pixel maps to the tone-mapping
  `peakNits` (reference white and peak are copied from the tone-mapping config each frame, so there is
  one source of truth). Sources above `glare_threshold_nits` (default 1e6, the sun disc and glints) are
  glare, not protected. The cap engages quickly (`highlight_attack_seconds`, 0.04 s) and releases at the
  normal photopic darkening time.
- **Natural boost limit**: the photopic gain may exceed the full-daylight setting
  (`daylight_adaptation_nits`, 50,000) by at most `max_boost_stops` (6), so dark interiors stay dark
  instead of being lifted to mid-gray.

The Display Diagnostics panel shows the brightest pixel in cd/m^2, the stops removed by protection and the
stops the boost limit refused. Settings persist as `display.eye.*`. The same controls and state are
available as `orbit_eye_get` / `orbit_eye_set` / `orbit_eye_reset` (`display.eye_*`).

Hot iteration: the eye update lives in the hot-reloadable post-process module (interface version 2), so
saving `HumanEyeAdaptation.cpp` swaps the implementation in-process; the new state fields default to
"no limit" so the running state survives the swap.

## Verifying

`tools/mcp_server/orbit_editor_mcp_server.py` can be imported and its tool
functions called directly for scripting and CI:

```python
import orbit_editor_mcp_server as m
m.orbit_project_create("C:/tmp/MoonTest", "Moon Test")
print(m.orbit_body_list())
```

## Hot iteration

The RPC methods live in the Studio host (`apps/editor`, `engine/studio_ui`,
`engine/editor_rpc`); saving those files takes the automatic Studio-generation
handoff described in `ORBIT_HOT_ITERATION.md`. The MCP adapter is a plain Python
script: restart the MCP server to pick up edits.
