# Tool reference

Arguments below are the `arguments` object of an MCP `tools/call` request.
The authoritative machine-readable schemas are returned by `tools/list`.

| Tool | Arguments | Result |
|---|---|---|
| `studio_status` | `{}` | Category, active body ID, dirty/apply scope, undo/redo availability, status text, live-world flag and viewport size. |
| `studio_ui` | Optional `include_hidden: boolean`, `scope: "editor"` or `"debug"` | Control rows with ephemeral ID, hierarchy path, type, visibility, text, tooltip, rectangle, value/range and option indices where applicable. |
| `studio_category` | `category` | Opens `PLANET`, `TERRAIN`, `WATER`, `ATMOSPHERIC` or `CELESTIALS`. |
| `studio_control` | `id`, `action`, optional `value` | Invokes a supported control operation. |
| `studio_session` | `action`, optionally `body_id` or `path` | Calls the editor's existing session action and returns status. |
| `studio_inspect` | Optional `path: array`, `depth: integer` (0–6, default 2) | Reads stored staged resource data. |
| `studio_texture_stack` | `action` (`get`/`set`), `biome_id`, optionally `layers: array` (`set`) | Structured read/replace of one biome's BIOME TEXTURE band stack, without hand-decoding raw graph nodes/links or driving each field through a separate `studio_control` call. |
| `studio_input` | `events: array` (1–120) | Injects each key/mouse event through Godot input, one per frame. |
| `studio_screenshot` | Optional `name`, `intent`, `tags`, `hide_editor`, `baseline` | MCP PNG and metadata; named captures persist full-resolution images and sidecars. |
| `studio_camera` | `action` and action-specific values | Move, rotate, slew, teleport, zoom or restore a pole-safe camera pose. |
| `studio_time` | `action` and action-specific values | Pause/play, signed timewarp, advance/rewind, seek or select a planetary date. |
| `studio_view` | `action` and action-specific values | Discover/select every engine debug view, full-bright, geomorph/render diagnostics, debug-menu visibility, and the runtime terrain-debug flags (wireframe, freeze, side-cut, etc.) directly. |
| `studio_captures` | `action`, optionally `name`, `id`, `baseline` | List/read named captures or persist an image comparison. |

See [vision commands and workflows](vision.md) for these tools' arguments and examples.

## Discovery and control operations

Use `studio_ui` after opening a category/tab or changing an item. IDs are opaque
strings based on live object identity. Do not save them between calls that can
rebuild the UI. Paths and neighboring label rows provide context for fields with
no own text, or buttons such as `Remove` that occur in multiple cards.

| Action | Control | Value |
|---|---|---|
| `press` | Button | Omit. Toggle buttons change state and emit their press callback. Use this for terrain tabs. |
| `set` | Range/SpinBox/slider | Finite number within the discovered min/max; the control applies its own step. |
| `set` | Toggle button | Boolean, emits the normal toggled signal if changed. |
| `set` | Editable LineEdit/TextEdit | String. LineEdit sends both change and submit signals; TextEdit sends change. |
| `select` | OptionButton/ItemList/MenuButton/TabContainer | Zero-based item/tab index from discovery. Disabled items and separators are rejected. |
| `color` | ColorPickerButton | `[red, green, blue, alpha]`, finite numeric components (HDR values allowed). |
| `curve` | CurveFieldControl | Flat `[x0,y0,x1,y1,...]`, 2–8 points, coordinates in `[0,1]`, strictly increasing x, first x=0 and last x=1. Emits `curve_changed`. |
| `file` | Open FileDialog | Path string. Dialog access restrictions apply, and open-file paths must exist. Sends `file_selected` to the existing importer/saver. |
| `scroll` | ScrollContainer | `[horizontal_pixels, vertical_pixels]`. |

Hidden, deleted, disabled and out-of-editor controls are rejected. All mutations
use the current callback; MCP does not assign arbitrary node properties or
execute arbitrary GDScript. Read-only text/numeric controls cannot be changed.

Example request (replace the ID with a fresh discovery result):

```json
{
  "jsonrpc": "2.0",
  "id": 42,
  "method": "tools/call",
  "params": {
    "name": "studio_control",
    "arguments": {"id": "123456789", "action": "set", "value": 0.6}
  }
}
```

## Sessions and model inspection

`studio_session.action` accepts `undo`, `redo`, `apply`, `revert`, `select_body`,
`save_preset` and `load_preset`. `select_body` requires `body_id`; inspect the
system's `bodies` array to find IDs. Preset actions require a `.tres` path under
`user://world_authoring/presets/`, without `..` traversal.

```json
{"action": "save_preset", "path": "user://world_authoring/presets/my-planet.tres"}
```

```json
{"path": ["bodies", 0, "planet_profile", "terrain"], "depth": 2}
```

Inspection begins at the staged celestial system. Path elements are resource
property names, dictionary keys or array indices. Only stored script properties
are exposed. At the depth limit collections return `{size, truncated}` and
resources return a type marker; use a deeper path to inspect them. Embedded
image bytes return a byte count. Vectors and colors become numeric arrays.

Apply scopes match `WorldAuthoringSession.ApplyScope`: 0=none, 1=hot, 2=graph,
3=tiles, 4=clipmap, 5=full rebuild. The session's apply callback is invoked
unchanged. A successful tool result does not claim that an asynchronous runtime
rebuild has finished.

## Structured biome texture editing

`studio_texture_stack` reads or replaces one biome's BIOME TEXTURE band stack as the
same layer-dict shape the editor itself builds and consumes, rather than requiring a
raw `studio_inspect` walk of the underlying `TEXTURE_BAND` graph nodes/links, or one
`studio_control` call per field.

```json
{"action": "get", "biome_id": 7}
```

Returns `{"biome_id", "layers", "imported_textures", "texture_choice_labels"}`. Each
layer has `texture_choice` (index into `texture_choice_labels`: Flat colour / Ground /
Grass / Mud / Forest / Imported — `custom_texture_index` selects which imported
texture when `texture_choice` is 5), `color`/`color_b`/`emission_color` as
`[r,g,b]`/`[r,g,b,a]`, `gradient_curve` as a flattened `[x0,y0,x1,y1,...]` point list
(2–4 points, matching `studio_control`'s `curve` action), height/slope/cavity ranges,
`height_relative`, `softness`, `opacity`, noise/tint fields, roughness/metallic/
anisotropy value+enabled pairs, and emission color/strength/enabled. `imported_textures`
lists each PBR library entry's name and tile size (image bytes are summarized as a
byte count, as with `studio_inspect`).

```json
{"action": "set", "biome_id": 7, "layers": [{"texture_choice": 2, "color": [0.2, 0.5, 0.15], "height_max": 900.0}]}
```

`set` replaces the entire stack (up to 8 bands) in one staged action — the same
undo/dirty/apply-scope semantics as editing bands by hand. A field left off a layer
falls back to the same default `+ Add texture band` uses, so a minimal object is
enough to add a plain band. This is the cheapest way to author several bands or
several fields on one band via MCP: `get`, edit the JSON, `set`, instead of
re-discovering IDs after every single field change (Biome Terrain/Texture pages
rebuild their controls on each edit — see `studio_ui`'s note above).

## Viewport editing

Select the desired tool/feature with editor controls, then use `studio_input`
for placement, dragging and navigation. Coordinates are viewport pixels from
`studio_status.viewport_size` and control rectangles. Screenshot width is capped
at 1600, so scale positions if the actual viewport is wider.

```json
{
  "events": [
    {"type": "button", "position": [1100, 450], "button": 1, "pressed": true},
    {"type": "motion", "position": [1120, 460], "relative": [20, 10], "button_mask": 1},
    {"type": "button", "position": [1120, 460], "button": 1, "pressed": false}
  ]
}
```

Key events use names understood by Godot, e.g. `W`, `Space`, `Ctrl`, `Shift`:

```json
{"events": [{"type": "key", "key": "W", "pressed": true}, {"type": "key", "key": "W", "pressed": false}]}
```

Mouse buttons are 1=left, 2=right, 3=middle, 4/5=wheel. Motion events accept
`relative` and `button_mask`; key/button events accept `pressed`. Button events
also accept `double_click`. All event types accept `shift`, `ctrl`, `alt`.
Always release held keys/buttons. For longer navigation send a press, subsequent
motion events and a release; each event advances a frame. Mouse events move the
cursor in a rendered window so the editor's per-frame picking follows the input.

## Example workflows

- **Global terrain:** open `TERRAIN`, discover and press `GLOBAL TERRAIN`, set
  the desired named numeric control, inspect status, apply.
- **Biome terrain:** press `BIOME TERRAIN`, select a biome, press `+ Add terrain
  layer`, re-discover, choose its layer type and adjust parameters/curve. Use
  the layer's arrow/Remove buttons for ordering/deletion.
- **Texture bands:** press `BIOME TEXTURE`, select a biome, press `+ Add texture
  band`, re-discover and set appearance, colors, masks and material overrides.
- **PBR import:** press `+ Import texture...`, discover the now-open FileDialog,
  send `file` with an existing albedo path, then use the imported card's
  roughness/normal import controls the same way.
- **Water authoring:** open `WATER`, use its feature creation/selection controls,
  set dimensions/flow values, then place points or handles with viewport input.
- **Celestials:** open `CELESTIALS`, create/select a body through controls, edit
  parent/orbit values, or select a star and edit its star-specific fields.

Check status/model after edits. Tool failures are MCP results with `isError:
true`; malformed protocol requests and unknown tools use JSON-RPC errors.
Transport failures are never retried automatically, avoiding duplicate edits.
