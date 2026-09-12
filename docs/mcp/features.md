# Feature coverage

The supported surface is the current Planet Studio UI, including controls
created after selecting a body, biome, layer or feature. MCP discovers these at
runtime rather than maintaining a second catalog of hundreds of editor fields.
Their current bounds, options, enabled state and callbacks remain authoritative.

| Editor area | Features available through MCP | Tools |
|---|---|---|
| Shared toolbar | Body selection, undo, redo, apply, revert, preset save/load, overflow menu, panel visibility, Back | `studio_session`, `studio_ui`, `studio_control` |
| Planet | Body identity and physical settings, generation settings, orbit/seasons, applicable per-body profile fields | Category `PLANET`, controls, inspection |
| Global Terrain | Coarse elevation controls; Surface Detail (microrelief, anti-tiling, procedural rock PBR, scanned ground/grass/mud/forest PBR incl. micro/macro tiling crossfade), Material Balance, Visual Character, Key Colors and Classification Rules for the shared surface classifier; repair controls when required, diagnostics | Category `TERRAIN`, `GLOBAL TERRAIN`, controls |
| Biome Terrain | Biome selection/pick-here, layer creation/removal/reordering, type-specific parameters, response curves, blend/composition, ported character defaults, clear actions and budget readouts | `BIOME TERRAIN`, controls, curves, viewport input |
| Biome Texture | Biome selection, band stacks, ordering/removal, appearance, height/slope conditions, gradients, response curves, patchiness and material overrides | `BIOME TEXTURE`, controls, colors, curves |
| Imported PBR textures | Import/replace albedo, roughness and normal maps, name/tile size, remove maps or entries | Texture library controls and open-dialog `file` operation |
| Water | Ocean settings, authored lake/river feature controls, geometry/knot/flow parameters, placement and handle editing exposed by the current page, runtime shader controls | Category `WATER`, controls, input |
| Atmospheric | Atmosphere/cloud settings and runtime material/shader fields exposed by the current page | Category `ATMOSPHERIC`, controls |
| Celestials | Body creation/duplication/deletion, parent relationships, orbit settings, rings, star profiles and system map interaction | Category `CELESTIALS`, controls, input; `select_body` |
| Viewport | Navigation/look, biome picking/painting and terrain/water placement or handles when armed by an editor control; custom widget interaction | `studio_input`, `studio_screenshot` |
| Camera and visual testing | Direct movement, rotation, roll, slew, FOV zoom, pose restore, named intent-tagged captures and persistent baseline comparisons | `studio_camera`, `studio_screenshot`, `studio_captures` |
| Time and render inspection | Advance/rewind, signed timewarp, planetary dates, full-bright, all engine debug views, geomorph modes and full runtime debug-menu controls | `studio_time`, `studio_view`, `studio_ui` with `scope:debug` |
| State and diagnostics | Staged resource inspection, session/apply state, current UI notes and diagnostic labels, screenshot | `studio_inspect`, `studio_status`, `studio_ui`, `studio_screenshot` |
| Structured texture editing | Read or replace a biome's entire BIOME TEXTURE band stack as one structured object instead of raw graph nodes or per-field controls | `studio_texture_stack` |

## Scope and semantics

This exposes the features available in the current game editor. Retired UI such
as the old Terrain Shader Lab is not resurrected, and controls absent for the
selected body cannot be invoked. The current terrain page deliberately replaces
older editor panels. MCP is not an unrestricted interface to historical private
methods from those older implementations.

All staged mutations keep the editor's normal history. Existing live-only edits
retain their live behavior. In particular, if using an editor variant with sculpt
controls, those write the runtime sparse layer immediately, so session undo does
not undo those strokes. Shader compilation and baking keep their existing status
and error reporting; read status/diagnostics after invoking those actions.

For a new feature built from ordinary buttons, fields, options, color pickers or
`CurveFieldControl`, no MCP registration is needed. Its controls appear in
`studio_ui` once the relevant panel is built. A new custom widget can use viewport
input; add a semantic control action when coordinate-independent operation is
desirable.

## Verification boundary

Automated integration tests instantiate the actual Planet Studio scene and cover
all five category surfaces, nested biome terrain/texture creation, texture color/import,
response curves, model inspection, text edits with undo/redo/revert, stale IDs,
invalid input and local authentication. They run headless, without a bound game
world. GPU previews, screenshots, production rebuild completion and physical
viewport sculpt/placement effects require a rendered live-game check.
