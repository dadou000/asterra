# Cyclic simulation transport

## Behavior
Replace the full-width Simulate control with a compact colored play/pause action, separate planetary UTC and local-time readouts, and a cyclic scrubber that follows and changes the shared simulation clock. Derive the UTC clock from the active body's strongest direct radiative emitter in the body-fixed frame, then apply the primary viewport's longitude-derived 24-zone offset for local time. Color the scrubber's full-day track as a smooth night, dawn, day, dusk, night sky gradient informed by the observer latitude, dominant star and authored atmosphere.

## Existing owner
- Module: `OrbitStudioUi` and `OrbitStudioSession`.
- Class/service: `SimulationControls` owns transport operations; `SimulationClock` owns canonical time and play state.
- Canonical state: the session simulation clock, the active body's authored rotation settings, and the primary viewport's observer position.

## Primary insertion point
- File: `engine/studio_ui/src/SimulationControlsUi.cpp`.
- Symbol/function: `SimulationControlsUi::Draw`.
- Reason: the existing shell band already owns transport presentation and its existing controls call `SimulationControls`.

## Secondary touch points
- `engine/studio_ui/include/orbit/studio_ui/SimulationControlsUi.hpp` for rotation-day context and scrubber state.
- `engine/editor_ui/include/orbit/editor_ui/EditorUi.hpp` and `engine/editor_ui/src/EditorUi.cpp` for optional fixed-width row controls, preserving current default sizing elsewhere.
- `apps/editor/src/Main.cpp` to provide active-body rotation context.
- `StudioTerrainRuntimeBridge::ObserverSite("studio.primary")` supplies the current body-fixed observer longitude without copying terrain snapshots; the transport stores no duplicate location.
- `CelestialLightingService::DominantDirectLightingAtBody` selects the same emitter for the viewport and transport, using the existing direct-light irradiance and occlusion model.
- `PanelContext::CanvasGradientRect` draws smooth gradient spans within the existing canvas; the transport supplies sky colors and keeps the clock as the only time authority.
- `docs/ORBIT_MCP.md` and `tools/mcp_server/orbit_editor_mcp_server.py` to expose local time in the existing `time.get` and `time.set` methods.

## Must not be implemented in
- A second clock, timeline authority, or UI-only simulation controller.
- Renderer-only planetary time state.

## Data/control flow
`world/clock -> dominant emitter body-fixed direction -> prime-meridian UTC; viewport observer -> longitude zone -> local clock and scrubber -> shared SimulationClock on edit`.

## Validation
- [ ] Existing clock remains canonical.
- [ ] Slider wraps within the active body's day and scrubbing updates the shared clock.
- [ ] Existing `time.*` RPC/MCP methods remain the automation path.
- [ ] Crossing a 15-degree zone boundary changes the local readout by one hour while preserving absolute simulation time.
- [ ] Prime-meridian noon tracks the dominant star overhead; UTC and Local are exposed separately in UI and RPC.
- [ ] The full scrubber track shows the sky's night-to-day-to-night cycle, with the time thumb visible across light and dark portions.
- [ ] Build the affected Studio UI/editor targets.
