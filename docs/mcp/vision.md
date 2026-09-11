# Camera and visual regression workflow

Planet Studio exposes direct camera, clock and rendering controls in addition to
pointer input. They work through the existing MCP connection; no extra setup is
required. Open the game's live Planet Studio for camera movement. The standalone
UI scene has no camera to move.

## Camera

Call `studio_camera` with one of these argument objects:

```json
{"action":"state"}
{"action":"move","offset_m":[20,5,100],"space":"camera"}
{"action":"move","offset_m":[0,100,0],"space":"world"}
{"action":"rotate","degrees":[15,-10,0]}
{"action":"zoom","fov_deg":35}
{"action":"teleport","position_m":[0,1100000,10000]}
{"action":"slew","position_m":[0,1100000,-10000],"duration_s":2}
```

`camera` offsets mean **right, camera-up, forward** in metres. `world` offsets and
absolute positions use the game's canonical double-precision coordinates, not
floating-origin render coordinates. Inspect the current state before choosing an
absolute destination; a different selected celestial body can have a nonzero
centre. Slew interpolates a straight path over 0–30 real seconds with eased start
and stop. Movement is collision-free, as expected for an editor camera.

Rotation values are relative yaw, pitch and roll in degrees. Yaw uses the
continuously transported surface heading. Pitch retains the editor's ±1.55-radian
limit. Zoom changes optical FOV (1–150 degrees); it does not move the camera.
Keyboard movement is suspended during an MCP slew. Space/Ctrl in manual editor
navigation continue to move radially up/down; camera-space MCP movement uses the
camera's up direction.

Save the entire `state` result and restore it with:

```json
{"action":"pose","pose":{"...":"paste the full studio_camera state here"}}
```

The pose contains double-precision position, transported north/up vectors,
yaw/pitch/roll in radians, FOV and body identity. Select the same body before
restoring it. Forward/right vectors and near/far distances are also reported for
inspection. Restoring a pose does not restore terrain edits, time or weather.

The camera frame is parallel-transported between radial directions rather than
switching north/east axes near a pole. Both keyboard/mouse navigation and MCP
commands use this frame. The gameplay camera no longer overwrites the editor's
orientation each frame, and keyboard translation has one owner.

## Time, rewind and dates

```json
{"action":"state"}
{"action":"pause"}
{"action":"advance","seconds":3600}
{"action":"advance","seconds":-3600}
{"action":"seek","seconds":0}
{"action":"rate","rate":-3600,"playing":true}
{"action":"date","year":2,"day":30,"hour":12}
```

These arguments go to `studio_time`. Rates are signed simulation seconds per
real second, from -10,000,000 to +10,000,000. Zero freezes the clock; `pause` and
`play` explicitly control playback. Advance, seek and date selection pause by
default; pass `playing:true` to continue playback after scrubbing.

Dates use the **active body's calendar**, not Gregorian dates: year is an integer
relative to the system epoch, day is a zero-based integer within that body's
year, and hour is in `[0,24)` scaled to its day length. Read `day_seconds` and
`year_days` from state. Negative years and seconds are supported. The manual
Orbit & seasons time-scale field now also accepts negative rates.

Rewind is a celestial-clock operation: orbital positions, sun direction and
seasonal lighting follow the selected time. It does **not** undo edits or replay
weather, water or physics backwards. A screenshot's time is not a full simulation
checkpoint.

## Full-bright and all debug views

```json
{"action":"state"}
{"action":"full_bright","enabled":true}
{"action":"full_bright","enabled":false}
{"action":"mode","mode":"normal_buffer"}
{"action":"mode","mode":"wireframe"}
{"action":"mode","mode":"disabled"}
{"action":"geomorph","index":1}
{"action":"render_toggle","key":"clouds","enabled":false}
{"action":"terrain_debug","key":"wireframe","enabled":true}
{"action":"terrain_debug","key":"sink_scale","value":3.0}
{"action":"terrain_debug","key":"reset"}
```

`terrain_debug` flips the runtime "ASTERRA DEBUG" terrain panel's own flags directly --
`wireframe`, `freeze_terrain`, `side_cut`, `stable_displacement`,
`stable_ocean_displacement`, `microrelief`, `pbr_detail`, `gpu_scatter`,
`agl_height_cursor`, `gpu_height_cursor`, `physics_height_cursor` take `enabled`;
`sink_scale`, `aerial_strength` take `value`; `key:"reset"` clears every
inspection flag/hold back to defaults. `studio_view {"action":"state"}` reports
current values under `terrain_debug`. Switching `mode` away from `"wireframe"`
also clears the terrain panel's own `wireframe` flag (and its HUD label), not
just the raw `Viewport.DebugDraw` buffer.

These arguments go to `studio_view`. State discovers **all** `Viewport.DebugDraw`
names from the running Godot version, including lighting, normals, wireframe,
overdraw, shadow atlases, GI, SSAO, SSIL, motion vectors and internal buffers. It
also returns terrain geomorph modes and every render-chain toggle. Some buffers
require a particular renderer or enabled effect; requesting a view does not
enable its underlying rendering feature automatically.

Full-bright is the engine's **unshaded-material diagnostic view**, which removes
ordinary lighting/shadow shading from material inspection. It does not change
the authored sun, atmosphere or materials. Disable it to restore the prior view.
Selecting another debug mode explicitly ends the full-bright override. It is not
a physically lit daylight preset; custom shader/compositor effects can remain.

For the complete game debug menu (terrain, render isolation, LODs/ring count,
sky, exposure, histogram and other available tabs):

1. `studio_view` → `{"action":"debug_menu","enabled":true}`.
2. `studio_ui` → `{"scope":"debug"}`.
3. `studio_control` → `select` on the discovered `TabContainer`, then re-query.
4. Use the same field/button actions as the editor. Close with `enabled:false`.

`studio_ui` defaults to `scope:"editor"`. The debug menu remains a separately
authorized UI subtree; other game nodes are not exposed as arbitrary controls.

## Named captures and comparisons

```json
{
  "name":"north-pole-seam",
  "intent":"Check that terrain remains continuous while crossing the north pole",
  "tags":["pole","baseline"],
  "hide_editor":true
}
```

Send this to `studio_screenshot`. A name requires an intent. Each capture gets a
unique timestamped ID, a full-resolution PNG and JSON sidecar under
`user://world_authoring/captures/`. Reusing the name creates another capture,
without replacing the previous one. The MCP result contains both an inline PNG
(at most 1600 pixels wide) and metadata with absolute file paths.

Metadata records intent/tags, UTC time, camera pose, selected body, celestial
clock, viewport/geomorph/render settings, editor dirty state, engine version,
image dimensions and whether the editor was hidden. `hide_editor` temporarily
hides only Planet Studio's overlay and restores its previous visibility. Close
the debug/pause menus separately when you want a clean scene capture.

Use `studio_captures` to track the sequence:

```json
{"action":"list","name":"north-pole-seam"}
{"action":"read","id":"ID_FROM_A_CAPTURE"}
{"action":"compare","id":"NEW_CAPTURE_ID","baseline":"BASELINE_CAPTURE_ID"}
```

Listing returns up to 100 newest matching captures. Comparison requires identical
image dimensions. It writes an absolute RGB difference PNG, reports mean
absolute RGB error in `[0,1]` and the fraction of pixels where any channel differs
by more than 5/255, and flags mismatched camera/time/view metadata. The baseline
relationship and metrics are retained in the newer capture's JSON sidecar. Each
baseline has a separate difference image. You can also pass `baseline` directly
to `studio_screenshot` to capture and compare in one call.

For a repeatable check:

1. Pause the clock and record a camera state, date and debug-view settings.
2. Create a named baseline with a concrete intent and tags.
3. Make the proposed edit. Return to the same body, restore the saved pose, seek
   to the same clock seconds and restore the same rendering settings.
4. Allow rebuilds/exposure to settle, then capture using the same name and the
   baseline ID. Compare both the images and metadata warnings.

Pixel differences quantify change; they do not automatically classify improvement
or regression. Clouds, water, exposure, temporal sampling and asynchronous
streaming may differ even at the same celestial time. Keep those effects fixed
or disable them with the debug helpers when testing unrelated terrain changes.

## Validation

```powershell
python -B -m unittest discover -s tools/planet_studio_mcp -v
godot --headless --path . --script tests/planet_studio_mcp.gd
godot --headless --path . --script tests/planet_studio_vision.gd
godot --path . --script tests/planet_studio_vision.gd --resolution 800x600
```

The vision test uses an isolated camera fixture, verifies movement/slew/zoom/pose,
both poles, signed clock/date control, debug/full-bright restoration, path
validation and known-image comparison metrics. A rendered run additionally saves
a PNG with intent and pose metadata for visual inspection. It is a small rendered
fixture, not a production-world visual regression test. In this checkout the
rendered fixture can wait at shutdown for the existing native weather pre-spin
worker even after reporting zero test failures.
