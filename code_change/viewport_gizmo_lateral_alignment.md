+++
path = "/editor/viewport/gizmo-camera-alignment"
title = "Viewport gizmo lateral alignment"
kind = "placement"
status = "implemented"
owner_module = "OrbitStudioUi"
summary = "Align transform gizmo projections and hit rays with the Studio renderer's horizontal camera basis so lateral camera turns do not make the gizmo drift or rotate at an apparent double rate."
keywords = ["gizmo", "viewport", "lateral", "camera", "projection", "basis"]
sources = [
  "engine/studio_ui/src/StudioViewportManipulatorUi.cpp",
  "engine/render_view/src/RenderView.cpp",
  "engine/celestial_far_render/src/FarBodyRenderer.cpp",
]
symbols = ["StudioViewportManipulatorUi::Handle", "ProjectToViewport", "ViewportRay"]
invariants = [
  "Studio's renderer horizontal screen-right is forward x up; manipulator display and pointer rays must use that same basis.",
  "Transform target positions and canonical rotation state remain owned by ViewportManipulator and ObjectStore.",
  "Keep the correction local to viewport gizmo presentation and interaction; do not add a second camera basis or mutate camera state.",
]
related = ["/editor/viewport", "/rules/placement"]
verify = [
  "Gizmo origin and rings stay attached to the selected object while yawing the camera left and right.",
  "The displayed rotation ring can still be hovered and dragged on its visible side.",
]
+++

# Viewport gizmo lateral alignment

## Behavior

Transform gizmo visuals track lateral camera turns at the same rate as the rendered scene and remain clickable at their visible positions.

## Existing owner

- Module: `OrbitStudioUi`.
- Owner: `StudioViewportManipulatorUi::Handle` builds screen-space handles and maps pointer coordinates to manipulation rays.
- Canonical state: `ViewportManipulator` and the authored transform properties.

## Primary insertion point

- File: `engine/studio_ui/src/StudioViewportManipulatorUi.cpp`.
- Apply the renderer's mirrored horizontal basis consistently to projected gizmo points and the inverse pointer-ray input.

## Must not be implemented in

- Camera navigation or camera state.
- A second transform/gizmo controller.

## Validation

- [x] The `OrbitStudioUi` library and full `OrbitStudio` Release target build successfully.
- [x] Projection and hit-ray X coordinates use matching renderer screen-right orientation.
- [ ] Live lateral-yaw inspection and a visual ring hover/drag confirmation remain for the next interactive session.
