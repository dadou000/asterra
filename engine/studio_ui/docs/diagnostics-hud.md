+++
path = "/editor/studio-ui/diagnostics-hud"
title = "Diagnostics HUD, display and eye controls, transport"
kind = "subsystem"
status = "stable"
summary = "The viewport text diagnostics HUD and its view.text_diagnostics RPC share one report; the Display Diagnostics panel owns luminance, eye adaptation, tone mapping, LUT, bloom and lighting-runtime controls (only eye adaptation has RPC/MCP so far); the Debug panel switches the GBuffer view; the transport band and time.* RPC drive the simulation clock."
owner_module = "OrbitStudioUi"
keywords = ["text diagnostics", "hud", "text readout", "view.text_diagnostics", "view.text_diagnostics_set", "display diagnostics", "eye adaptation", "display.eye_get", "display.eye_set", "display.eye_reset", "exposure", "highlight protection", "debug view", "surface debug", "gbuffer", "simulation controls", "transport", "time.set", "time.step", "latitude longitude", "height above datum"]
sources = [
  "engine/studio_ui/include/orbit/studio_ui/StudioTextDiagnosticsHud.hpp",
  "engine/studio_ui/src/StudioTextDiagnosticsHud.cpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioViewportTextDiagnostics.hpp",
  "engine/studio_ui/src/StudioViewportTextDiagnostics.cpp",
  "engine/studio_ui/src/StudioRenderViewSet.cpp",
  "engine/studio_ui/src/StudioRenderViewRpc.cpp",
  "engine/studio_ui/include/orbit/studio_ui/DisplayDiagnosticsUi.hpp",
  "engine/studio_ui/src/DisplayDiagnosticsUi.cpp",
  "engine/studio_ui/src/DisplayDiagnosticsUiBase.cpp",
  "engine/studio_ui/include/orbit/studio_ui/DisplayEyeRpc.hpp",
  "engine/studio_ui/src/DisplayEyeRpc.cpp",
  "engine/studio_ui/include/orbit/studio_ui/DebugViewUi.hpp",
  "engine/studio_ui/src/DebugViewUi.cpp",
  "engine/studio_ui/include/orbit/studio_ui/SimulationControlsUi.hpp",
  "engine/studio_ui/src/SimulationControlsUi.cpp",
  "tools/mcp_server/orbit_editor_mcp_server.py",
]
symbols = ["StudioTextDiagnosticsHud", "FormatStudioViewportTextReport", "SampleStudioTerrainPoint", "StudioPixelFootprintMeters", "DisplayDiagnosticsUi", "ApplyDisplayDefaults", "RegisterDisplayEyeRpc", "kNumberFields", "DebugViewUi", "SimulationControls", "SimulationControlsUi", "RegisterSimulationRpc"]
invariants = [
  "The HUD and view.text_diagnostics show one report: both build StudioRenderViewSet::TextDiagnostics(id, cursor) and format it with FormatStudioViewportTextReport, and the RPC's `text` field is exactly that string (its `hud` field says whether the HUD is on).",
  "The HUD is a per-view flag in StudioRenderViewSet (SetTextDiagnosticsHud / TextDiagnosticsHud, off until set; an unregistered view id throws). The viewport Diagnostics 'Text readout' checkbox (StudioExpansionShell.cpp) and view.text_diagnostics_set / orbit_view_text_diagnostics_set change the same flag.",
  "StudioTextDiagnosticsHud::Draw must be called right after the viewport Image is submitted (the text is anchored with OverlayTextOnLastItem). Text is rebuilt only when the 11-value key changes (camera position, forward, cursor u/v or -1 when not hovered, view width/height, vertical fov) or after 500 ms, because the report samples the terrain source about a hundred times; a thrown error is shown as 'Diagnostics unavailable: ...'.",
  "Terrain lines exist only while the target's terrain runtime is current: navigation is then 'terrain'; otherwise it is 'reference_sphere' (a target exists) or 'none', hasTerrain is false and the formatted text stops after 'Distance from core ... (no terrain on this target)'.",
  "Coordinates follow one convention: +Y is the pole, latitude = asin(direction.y), longitude = atan2(direction.z, direction.x), degrees; heading 0 is north. Heights are metres: height above datum = distance from core minus planet radius, above terrain = distance from core minus the nadir ground radius, and above water surface is reported only when the nadir point is underwater.",
  "The point below the camera is sampled with footprint StudioPixelFootprintMeters(fov, view height, max(|height above datum|, 1)), never below 0.25 m; slope and downhill bearing come from a central difference on the same terrain source, so they reflect only the relief resolved at that footprint.",
  "display.eye_get / display.eye_set / display.eye_reset (RegisterDisplayEyeRpc) act on a view id that defaults to studio.primary and need luminance metering for that view (otherwise error 1072). eye_set changes only the given fields, rejects a non-boolean highlight_protection or any of the 14 numeric fields in kNumberFields that is non-finite or outside its range (error -32602), and is not persisted to the display settings file.",
  "The Debug panel (DebugViewUi), each viewport's Surface View buttons and view.surface_debug_set all call StudioRenderViewSet::SetSurfaceDebugMode; the modes are lit, base_color_roughness, normal_metallic and emission_metadata.",
  "SimulationControls is the one owner of the transport: the bottom band (SimulationControlsUi, id 'orbit.simulation') and time.get / time.set / time.step all call it. time.set applies rate, step_seconds, time_microseconds, then playing; step size is positive and finite (default 60 s); time_microseconds beyond +-9.0e18 is rejected.",
  "DisplayDiagnosticsUi.cpp #includes DisplayDiagnosticsUiBase.cpp with `#define Register RegisterBase` and `#define DrawViewport DrawViewportBase`; the Base file is not a CMake source. Per-view luminance, eye, LUT, output, tone mapping and bloom sections live in the Base file; lighting runtime override, lighting inspection overlays and the lighting/volume profiler live in the wrapper, and project defaults are applied to both studio.primary and studio.map by ApplyDisplayDefaults.",
]
related = ["/editor/viewport", "/editor/mcp-rpc", "/editor/profiler", "/rendering/post-process", "/rendering/lighting", "/rendering/volumes", "/foundation/time", "/rendering/terrain/clipmaps/debugging", "/editor/studio-ui/reports", "/editor/studio-ui/validation-and-capture"]
depends_on = ["/editor/viewport", "/rendering/post-process", "/foundation/time"]
used_by = ["/editor/studio-ui", "/editor/studio-ui/reports"]
verify = [
  "Over MCP: orbit_view_text_diagnostics returns `text` identical to what the HUD shows (compare after orbit_view_text_diagnostics_set enabled=true).",
  "orbit_eye_get, then orbit_eye_set with one field, then orbit_eye_get again: only that field of `config` changes; orbit_eye_reset returns reset=true.",
  "orbit_time_set playing=false then orbit_time_step: time_microseconds advances by step_seconds while paused.",
  "ctest -R Orbit.StudioViewportPresentation and Orbit.LightingSelectionInspection cover related viewport and lighting-inspection code; the HUD formatter, eye RPC and transport have no dedicated test target, so use the RPC checks above.",
]
verified = "55d48117"

[routes]
"terrain overlay or clipmap numbers look wrong" = "/rendering/terrain/clipmaps/debugging"
"camera, navigation or the viewport Diagnostics toggles" = "/editor/viewport"
"exposure, tone map, LUT, HDR output behaviour itself" = "/rendering/post-process"
"validation scenario that resets the eye or enables lighting overlays" = "/editor/studio-ui/validation-and-capture"

[[diagnose]]
symptom = "the text HUD is blank, stale or lacks terrain lines"
steps = [
  "Call view.text_diagnostics {id}: `hud` false means the flag is off (view.text_diagnostics_set {id, enabled: true}); a 'View ... not registered' style error means a wrong view id.",
  "Read `navigation` in the result: 'reference_sphere' or 'none' means the terrain runtime is not current for that view, so the text ends after the distance from core by design.",
  "The HUD refreshes at most every 500 ms unless the camera or hovered cursor changes; pass cursor_u and cursor_v (both in [0,1]) to include the 'Under cursor' block.",
]
docs = ["/editor/viewport", "/rendering/terrain/clipmaps/debugging"]

[[diagnose]]
symptom = "the image is too dark, too bright or glare keeps changing exposure"
steps = [
  "Call display.eye_get {id}: compare state.brightest_pixel_nits, state.adapted_nits, state.highlight_protection_stops and state.boost_limit_stops with config.highlight_protection, glare_threshold_nits, daylight_adaptation_nits and max_boost_stops.",
  "After a camera cut, call display.eye_reset so the next frame adapts instantly instead of easing.",
  "If display.eye_get fails with 'has no luminance metering yet', the view has produced no luminance data: check the id and that the view has rendered frames.",
  "display.eye_set changes are not saved; project defaults come from Project Settings and are re-applied by ApplyDisplayDefaults.",
]
docs = ["/rendering/post-process"]

[[diagnose]]
symptom = "the simulation does not advance, or advances at the wrong speed"
steps = [
  "Call time.get: check playing, rate (simulation seconds per real second; negative runs backwards) and time_microseconds.",
  "time.set {playing: true} starts it; time.step {seconds} advances by that amount while playing or paused (default is step_seconds).",
]
docs = ["/foundation/time"]

[[diagnose]]
symptom = "a Display Diagnostics control cannot be reached by an agent"
steps = [
  "Only the eye-adaptation controls have RPC/MCP (display.eye_*). A search of the `.name = \"...\"` registrations in engine/ and apps/ found no RPC method for the metering, tone mapping, color LUT, output transform, bloom/glare, lighting runtime override or lighting overlay controls of the panel.",
  "AGENTS.md requires parity: extract the operation the control runs, register an RPC method, add a tool in tools/mcp_server/orbit_editor_mcp_server.py and document it in docs/ORBIT_MCP.md in the same change.",
]
docs = ["/editor/mcp-rpc"]
+++

## Where things live

| Concern | Owner |
| --- | --- |
| HUD drawing and its 500 ms cache | `StudioTextDiagnosticsHud` (one instance per host; viewport panels call `textHud_.Draw`) |
| report struct, terrain point sampling, formatting | `StudioViewportTextDiagnostics.cpp` (`SampleStudioTerrainPoint`, `FormatStudioViewportTextReport`) |
| building the report for a view | `StudioRenderViewSet::TextDiagnostics` |
| `view.text_diagnostics`, `view.text_diagnostics_set`, `view.surface_debug_get/set` | `StudioRenderViewRpc.cpp` |
| luminance histogram, eye state, LUT, tone mapping, bloom, lighting override panel | `DisplayDiagnosticsUi` (+ `DisplayDiagnosticsUiBase.cpp`) |
| `display.eye_*` | `DisplayEyeRpc.cpp` |
| GBuffer channel toolbar | `DebugViewUi` |
| transport band, `time.*` | `SimulationControls`, `SimulationControlsUi`, `RegisterSimulationRpc` |

## What the text report contains

Per view: view id, mode (perspective, body_map, debug, system, flat_map), size and navigation; camera position, heading,
pitch, fov, near and far; the terrain layer flags and LOD bias; planet radius, distance from core and heights; for the
point below the camera and (when hovered) under the cursor, latitude and longitude, terrain and coarse elevation, detail
delta, water depth, radius from core, slope, downhill bearing, climate and biome weights (weights below 0.0005 are
omitted); clipmap plan, CPU terrain page and patch counters, and the terrain runtime page level, coverage tier, source
revision and world and runtime generations. Cloud data is in the struct and RPC but not in the formatted text.

## RPC and MCP

| RPC | MCP tool |
| --- | --- |
| `view.text_diagnostics` | `orbit_view_text_diagnostics` |
| `view.text_diagnostics_set` | `orbit_view_text_diagnostics_set` |
| `view.surface_debug_get` / `view.surface_debug_set` | `orbit_view_surface_debug_get` / `orbit_view_surface_debug_set` |
| `display.eye_get` / `display.eye_set` / `display.eye_reset` | `orbit_eye_get` / `orbit_eye_set` / `orbit_eye_reset` |
| `time.get` / `time.set` / `time.step` | `orbit_time_get` / `orbit_time_set` / `orbit_time_step` |

## Display Diagnostics panel notes

The wrapper's "Lighting Runtime Override" is a session override (hardware ray query, emissive GI quality clamped to
0..4, per-section GPU budgets floored at 0); Project Settings owns the persisted defaults. The inspection overlays
clamp maximum GI cells to 1..256 and cache levels to 1..8. They are transient inspection state and do not alter
lighting authority.
