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

`ORBIT_RPC_HOST`, `ORBIT_RPC_PORT`, `ORBIT_RPC_TIMEOUT` override the defaults.
The bridge opens one short connection per call, so a Studio relaunch never
leaves it holding a dead socket.

### In-app feedback

Every mutating MCP/RPC command shows a toast in the lower-right corner of
Studio (`MCP: <method>`), and every failed command (mutating or not) shows a red
toast with the error. Read-only queries stay silent so polling does not flood
the screen. See `RecordRpcNotifications` in `apps/editor/src/Main.cpp` and
`EditorUi::PushNotification`.

## Projects: create, open, switch

| MCP tool | RPC method | Notes |
| --- | --- | --- |
| `orbit_project_info` | `project.info` | Open project id, name, root, startup world. |
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
| `orbit_view_terrain_overlays_get/_set(view_id, flags...)` | `view.terrain_overlays_get` / `view.terrain_overlays_set` | Terrain diagnostic overlays: dirty_page_bounds, build_states, physical_lod, clipmap_rings (outlines the active clipmap levels only), clipmap_levels (tints the terrain by clipmap level), clipmap_wireframe (terrain as wireframe), clipmap_freeze (freezes the clipmap so the camera can fly away and inspect it), cache_status, authored_constraints, biome_weights, process_masks, drainage_vectors. Same toggles as the viewport Diagnostics properties. |
| `orbit_view_terrain_layers_get/_set(view_id, production_surface?, full_clipmap?, macro_globe?, ocean?, surface_effects?, lod_bias_stops?, dynamic_clipmaps?, clipmap_pixels_per_vertex?, clipmap_fade_seconds?, experimental_distance_bands?, clipmap_band_edges_meters?, clipmap_band_scale?, clipmap_partial_updates?, physical_pages?, clouds?)` | `view.terrain_layers_get` / `view.terrain_layers_set` | Which terrain layers a viewport draws (near-field clipmap terrain, orbital globe patches, ocean, surface effects) and its LOD bias in stops (clamped to [-4, 4]; +1 keeps richer representations longer and doubles orbital patch resolution). `full_clipmap` (default on) draws the production clipmap from the ground to orbit and never uses the orbital globe. `dynamic_clipmaps`, `clipmap_pixels_per_vertex` and `clipmap_fade_seconds` (how long a level dissolves in or out, default 0.4 s, 0 = pop) (and the EXPERIMENTAL `experimental_distance_bands` with its `clipmap_band_edges_meters` list and `clipmap_band_scale` multiplier: clipmap level k is drawn only where the camera's distance to the terrain is in its band, neighbours cross-fading) control the dynamic clipmap planner (see [ORBIT_PERFORMANCE.md](ORBIT_PERFORMANCE.md)). `clouds` (default on) ray-marches the body's cloud layer in the viewport (see [ORBIT_PERFORMANCE.md](ORBIT_PERFORMANCE.md)). Same controls as the viewport Diagnostics "Terrain layers" section. |
| `orbit_view_text_diagnostics(view_id, cursor_u?, cursor_v?)` | `view.text_diagnostics` | Complete numeric and text diagnostic for a viewport: camera position/heading/pitch, distance from the planet core, height above datum (sea level), above terrain and above the water surface, plus for the point below the camera and (with cursor_u/cursor_v) under the cursor: latitude/longitude, terrain and coarse elevation, detail delta, water depth and surface, radius from core, slope, downhill bearing, climate and biome weights, physical page/LOD, terrain runtime revisions, and `cpu_terrain` (physical-page pool workers/running/queued, page rebuild counts and products, orbital patches building/resident), and `clipmap_plan` (which clipmap levels the planner keeps active: first/last level, finest spacing, coarsest reach, nearest ground, the spacing the screen asks for, plan changes) and `clouds` (the target body's built cloud field: layer count, mean coverage and optical depth, time bucket, whether it is GPU-resident; absent without a cloud layer). `text` is exactly what the HUD shows. |
| `orbit_view_text_diagnostics_set(enabled, view_id)` | `view.text_diagnostics_set` | Shows or hides that HUD over the viewport (the "Text readout" checkbox in the viewport Diagnostics properties). |
| `orbit_profiler_status` | `profiler.status` | CPU micro-profiler: configuration, frame-time summary, last 120 frame times and recent hitch captures (file path, frame ms, sampled stack frames). See [ORBIT_PROFILER.md](ORBIT_PROFILER.md). |
| `orbit_profiler_configure(enabled?, hitch_threshold_ms?, stall_threshold_ms?, capture_window_ms?, max_hitch_files?)` | `profiler.configure` | Turns the profiler on/off and sets the hitch and stall thresholds and capture window. |
| `orbit_profiler_capture(window_ms?, path?)` | `profiler.capture` | Writes the last few seconds of every thread and CPU core as a Perfetto trace and returns its path. |
| `orbit_profiler_panel_get` | `profiler.panel_get` | State of the Profiler panel (View menu > Profiler): paused/live, snapshot source, options, visible range, selected slice. |
| `orbit_profiler_panel_set(paused?, window_ms?, grouping?, freeze_on_hitch?, min_slice_ms?, filter?, reset_view?, view_begin_ms?, view_span_ms?, zoom_frame?, select_thread?, select_time_ms?, clear_selection?, zoom_to_selection?, load_trace?)` | `profiler.panel_set` | Everything the panel's controls do: Pause / Resume live, history window, by-thread or by-core lanes, pause-on-hitch, hide short slices, highlight a scope, zoom and pan, pick a frame or slice, open a saved hitch trace. |
| `orbit_profiler_snapshot(top_scopes?, slowest?)` | `profiler.snapshot` | Analysis of the visible range of the panel's snapshot: frame stats, lane busy time, heaviest scopes, heaviest GPU passes (`top_gpu_passes`), longest slices with thread/core/self time, stall stack samples. |
| `orbit_view_mode_set(mode, view_id?)` / `orbit_view_debug_field_set(field?, view_id?)` | `view.mode_set` / `view.debug_field_set` | Viewport mode (perspective, body_map, debug, system) and, in debug mode, which terrain field is shown (omit `field` to list names). Same operations as the viewport mode selector and Debug tab. |
| `orbit_workspace_get` / `orbit_workspace_set(mode)` | `studio.workspace_get` / `studio.workspace_set` | Scene, Planet, Celestial, Simulation, Shading; same as the Mode selector. |
| `orbit_bubble_open(object_id)` | `studio.bubble_open` | Opens the element parameter bubble shown in the active mode toolbar. |

The Shading tab (content tree with folders, live-compiled HLSL shaders and
shader materials, a preview on selectable shapes and lighting) is fully
scriptable through `shading.*`; see [ORBIT_SHADING.md](ORBIT_SHADING.md) for the
contract, the hot path (shader edits never restart Studio) and the complete
tool list.

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
| `orbit_schema_catalog` / `orbit_command_catalog` | `schema.catalog` / `command.catalog` |
| `orbit_transaction_begin/commit/rollback`, `orbit_undo`, `orbit_redo` | `transaction.*`, `history.*` |
| `orbit_selection_get/set/clear` | `selection.*` |
| `orbit_viewport_get/set_camera/screenshot/focus_body` | `viewport.*` |

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

Then `selection.set` the Moon and take a `viewport.screenshot`: the Moon renders
as a grey, sunlit disc.

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
