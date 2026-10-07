+++
path = "/editor/studio-ui/validation-and-capture"
title = "V0.0.7 validation scenarios and viewport capture"
kind = "subsystem"
status = "stable"
summary = "Thirteen V0.0.7 validation scenarios are registered as Explorer context-menu commands that only prepare a scene; ViewportCaptureService makes single and tiled 16K screenshots over viewport.capture_*. Real-GPU captures, hardware performance records and HDR output smoke tests are still not done."
owner_module = "OrbitStudioUi"
keywords = ["v0.0.7", "validation", "m43", "m44", "m46", "scenario", "rt a/b", "viewport capture", "screenshot", "16k", "ultra", "tiled capture", "viewport.capture_start", "hardware evidence", "hdr smoke", "validation commands"]
sources = [
  "engine/studio_ui/include/orbit/studio_ui/V007ValidationScenarios.hpp",
  "engine/studio_ui/src/V007ValidationScenarios.cpp",
  "engine/studio_ui/src/V007ValidationCommands.cpp",
  "engine/studio_ui/include/orbit/studio_ui/ViewportCaptureService.hpp",
  "engine/studio_ui/src/ViewportCaptureService.cpp",
  "engine/studio_ui/include/orbit/studio_ui/VolumeAuthoringUi.hpp",
  "tools/mcp_server/orbit_editor_mcp_server.py",
]
symbols = ["kV007ValidationScenarios", "V007ValidationCommandId", "PrepareV007ValidationScenario", "V007ValidationCommandRegistration", "ViewportCaptureService", "RegisterViewportCaptureRpc", "kMaxSinglePixels", "kTileLongSide", "kUltraLongSide"]
invariants = [
  "The 13 scenarios in kV007ValidationScenarios (LED Room, Cloud Glare, Dark Interior -> Daylight, Headlight / Brake Light, City Night Flight, Ground -> Orbit, RT A/B, Smoke Obstacle / Advection, Surface Dust / Wind, Emissive Fire GI, Roaming Domain Continuity, Live -> Baked Playback, Near -> Far Volume LOD) all have gpuVisualToleranceRequired = true: preparing one is not evidence that the GPU image is correct.",
  "PrepareV007ValidationScenario only sets a scene up (through production authoring commands, the lighting runtime config, renderer eye and volume settings, and the native cache bake) and returns a description string; it captures nothing and asserts nothing. The command wrapper logs that string with log::Info and does not return it.",
  "Each scenario is a command named 'Validate: <scenario>' in category 'Validation / V0.0.7', automationVisible = true, on the Explorer context-menu surface, enabled only when a world is open. Its id is V007ValidationCommandId: fixed high word 0x4f524249544d3433, low word 0x56414c0000000001 plus the enum index.",
  "V007ValidationCommandRegistration registers only when constructed with a session, a renderer and an open world; it skips ids already in the registry and unregisters only the ones it installed. VolumeAuthoringUi owns one instance (validationCommands_), so there is no separate validation app or test-only editor path.",
  "Setup is not neutral: the Headlight / Brake Light rig creates two spot lights and one point light in an undoable transaction (rolled back on failure when this call owns it); the six volume scenarios create Volume objects (Smoke, Dust, Fire presets) and select them; RT A/B turns hardwareRayQueryEnabled off; LED Room, City Night Flight, Ground -> Orbit and the lighting scenarios switch on the global lighting overlays.",
  "ViewportCaptureService renders in one piece up to kMaxSinglePixels (7680 x 4320 pixels); anything larger is tiled with tiles of long side kTileLongSide (3840), neighbours kTileStep (0.875) of a tile apart. While tiling it pauses the simulation and locks exposure, and Finish/ReleaseHolds restore the camera, exposure, clock and view size even after an error; waits are bounded (600 frames each for resize, busy and restore).",
  "The viewport.capture_start RPC and the viewport Screenshot / Ultra buttons call the same ViewportCaptureService::Start; it throws if a capture is already running, and the RPC maps a bad request to -32602 and other failures to 1110. kind is fullscreen (window size), ultra (15360 px long side, viewport aspect, max 16384) or custom (16..16384 each side).",
]
related = ["/history/v0-0-7", "/legacy/v0-0-7-m43-validation", "/legacy/v0-0-7-m44-performance-capture", "/legacy/v0-0-7-m46-integration-gate", "/legacy/v0-0-7-progress", "/editor/profiler", "/editor/mcp-rpc", "/editor/model", "/editor/studio-ui/reports", "/editor/studio-ui/diagnostics-hud"]
depends_on = ["/rendering/lighting", "/rendering/volumes", "/rendering/render-view", "/editor/model"]
used_by = ["/editor/studio-ui"]
verify = [
  "ctest -R Orbit.V007DeterministicValidation (CPU invariants and the 13-scenario catalog)",
  "ctest -R Orbit.V007IntegrationGate (depends on the other V0.0.7 gates)",
  "ctest -R Orbit.ViewportCaptureTiling",
  "None of these is GPU evidence. Over MCP: orbit_command_catalog shows the 'Validate: ...' commands; orbit_viewport_capture_start then orbit_viewport_capture_status until state is idle.",
]
verified = "55d48117"

[routes]
"what V0.0.7 still needs before it can be called validated" = "/history/v0-0-7"
"the expected scenario matrix and GPU tolerance rules" = "/legacy/v0-0-7-m43-validation"
"the required hardware matrix for performance captures" = "/legacy/v0-0-7-m44-performance-capture"
"a bug report that needs a screenshot attached" = "/editor/studio-ui/reports"

[[diagnose]]
symptom = "the 'Validate: ...' commands are missing from the Explorer menu or from command.catalog"
steps = [
  "The commands exist only if the registration was constructed while a world was open (V007ValidationCommandRegistration returns early otherwise); open a world, then re-check with command.catalog.",
  "Each command is disabled with reason 'Open a world before preparing validation scenarios.' when no world is open.",
  "If the log shows 'M43 validation command registration failed', read the exception text printed with it.",
]
docs = ["/editor/model"]

[[diagnose]]
symptom = "a scenario was invoked but nothing visible changed"
steps = [
  "The command returns nothing to the caller; its description string is only in the Studio log (log::Info). Read the log for the scenario's message.",
  "Some scenarios only enable overlays or reset eye state (LED Room, City Night Flight, Ground -> Orbit, Cloud Glare, Dark Interior -> Daylight); they rely on an authored scene already being open.",
  "Volume scenarios report 'Smoke preset creation did not yield a selected Volume.' style messages when the create command did not leave one Volume selected.",
]
docs = ["/rendering/volumes"]

[[diagnose]]
symptom = "viewport.capture_start fails, never finishes, or the view stays resized"
steps = [
  "Call viewport.capture_status: states are idle, resizing, settling (metering when tiled), tiles and restoring; `last` holds path, width, height, file_bytes or error.",
  "Error 1110 with 'already running' means wait for idle; -32602 means a bad kind or custom size outside 16..16384.",
  "Resize, busy and restore waits are capped at 600 frames each, after which the service finishes and restores the view; read `last.error`.",
]
docs = ["/editor/viewport"]
+++

## Validation scenarios (M43)

`V007ValidationScenarios.cpp` defines what each scenario sets up; `V007ValidationCommands.cpp` exposes them as commands.
A scenario is a starting position for a human or an agent to inspect, not a pass/fail check. Examples taken from the
code: Cloud Glare sets the eye photopic ceiling to 2.0 log2 and shortens ceiling and overload recovery; RT A/B turns
hardware ray query off (re-enable it in Display Diagnostics for the B capture); Live -> Baked Playback bakes a 16^3
native cache and forces the Volume to Baked; Near -> Far Volume LOD sets live distance 30 m, passive distance 180 m,
projected-pixel thresholds 96 and 12 and hysteresis 0.12.

Run over RPC/MCP: `command.catalog` (MCP `orbit_command_catalog`) lists the commands and their ids; run one with
`command.invoke` (MCP `orbit_invoke_command` or `orbit_invoke_command_by_name`, using the name 'Validate: RT A/B' and so on).
The deterministic numerical gates are the CTest targets listed under `verify`.

## Viewport capture

`ViewportCaptureService` is the high-resolution screenshot machine for the primary viewport. The default folder is
`<project>/Screenshots` and the default file name is `<yyyymmdd-hhmmss>-<kind>-<w>x<h>.png`; an explicit `path` ending in `.bmp` gives a BMP.
Default settle is 6 frames (single) or 20 frames (tiled), plus 8 frames per tile. A 16:9 Ultra capture (15360 x 8640) is
always tiled.

| RPC | MCP tool |
| --- | --- |
| `viewport.capture_start` | `orbit_viewport_capture_start` |
| `viewport.capture_status` | `orbit_viewport_capture_status` |
| `viewport.screenshots_open` | `orbit_viewport_screenshots_open` |

A simpler one-shot capture, `viewport.screenshot` (MCP `orbit_viewport_screenshot(path)`), is registered separately in `EditorRpcService`; reports use their own hook (see `/editor/studio-ui/reports`).

## What is still open for V0.0.7 (not done)

`docs/V0.0.7_PROGRESS.md` and `/history/v0-0-7` state that M00-M46 are implemented but release validation is open. Not done, and not to be claimed:

- Windows/Vulkan build and the full CTest run on the release commit, including `Orbit.V007IntegrationGate`;
- Vulkan validation-layer runs over the M43 scenario matrix;
- representative non-RT and RT-capable hardware performance captures (M44) with identical authored settings, and an RT A/B on identical hardware;
- M43 GPU image-tolerance captures;
- SDR and available HDR10 output smoke tests;
- destructive save/reopen followed by a steady-state GPU convergence comparison;
- a root `Orbit.exe` verified as version 0.0.7 with the stamped commit.

The CPU tests above prove deterministic invariants and the report schema only; none of them is hardware evidence.
