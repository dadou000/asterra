# Multi-body viewport rendering

## Behavior
Perspective Studio viewports show the other bodies in the active celestial system, including radiative stars and moons, at their simulated positions and apparent sizes. Bodies are composited around the active presentation in far-to-near order.

## Existing owner
- Module: `OrbitStudioUi` and `OrbitCelestialFarRender`.
- Class/service: `StudioViewportRenderer::Compose` schedules viewport passes; `FarBodyRenderer` shades distant bodies.
- Canonical state: `UniverseComposition` body/frame geometry, authored appearance bindings, and the shared `SimulationClock` time.

## Primary insertion point
- File: `engine/studio_ui/src/StudioViewportRenderer.cpp`.
- Symbol/function: `StudioViewportRenderer::Compose`.
- Reason: this is where active-body-only passes are selected and rendered. Background bodies belong in the same render graph and camera.

## Secondary touch points
- `FarBodyRenderer` accepts an off-centre projected location for disc and point representations while retaining its existing body shading.
- Existing world-model appearance and lighting bindings provide star color, luminosity, giant/small-body parameters, and incident light.
- A focused headless geometry test and full Studio build validate the path.

## Must not be implemented in
- Editor UI overlays or a second celestial clock/position cache.
- An independent rendering loop outside the Studio render graph.

## Data/control flow
`UniverseComposition + simulation time -> FrameGraph camera-relative body positions -> farther body draws -> active body -> closer body draws -> presentation`.

## Validation
- [x] The live Earth viewport renders the authored Sun as a body when atmosphere is bypassed; the atmosphere setting was restored after capture.
- [x] FrameGraph supplies each body's position and orientation; the far renderer sizes disc/point images by apparent angular radius and shades authored stellar, giant, small-body and material properties.
- [x] Far-to-near ordering puts bodies beyond the active body behind its terrain/globe and closer bodies after the active presentation.
- [x] The Studio Release build and celestial validation scenario pass. The existing native-generation watcher provides save-to-reflect for these C++ files.
- [ ] A dedicated real-device visual pass for a moon transit and a giant/ringed body remains to be captured.

## Camera alignment correction
- `RenderView::ProjectToViewport` uses `up x forward` as screen-right, while Studio's sky and far-body shaders use `forward x up`. The multi-body screen centre must mirror the projected horizontal coordinate to stay fixed against the rendered sky during camera yaw.
- Validate with a live viewport yaw comparison and an incremental Studio build; this remains on the existing native-generation reflection path.
