+++
path = "/editor/viewport/frame-selected"
title = "Frame Selected camera (placement note)"
kind = "placement"
status = "implemented"
owner_module = "OrbitStudioUi"
summary = """
Worked example of a placement note: a smooth 2-second Frame Selected camera transition owned by \
StudioRenderViewSet's navigation pose (not by the toolbar and not by the transient RenderView \
camera), triggered by the Scene toolbar, the F shortcut and an editor command that RPC/MCP share."""
keywords = ["frame selected", "camera", "framing", "transition", "toolbar", "navigation pose", "focus"]
sources = [
  "engine/studio_ui/src/StudioViewportPanels.cpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioRenderViewSet.hpp",
  "engine/studio_ui/src/StudioRenderViewSet.cpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioQol.inl",
]
symbols = ["StudioViewportPanels::DrawSceneToolbar", "StudioRenderViewSet", "NavigateTerrain"]
invariants = [
  "Canonical camera state is the navigation pose (StudioViewPose), not RenderView::Camera(); Refresh() rebuilds the rendered camera every frame.",
  "The panel may compute or request the framing target but must not hold the animation state.",
  "No FrameCameraController, second free camera or parallel navigation system.",
  "Toolbar, F shortcut, command palette and RPC/MCP all reach one operation.",
  "Any meaningful manual navigation input cancels an active transition; re-triggering starts a new transition from the current pose.",
]
related = ["/rules/placement", "/editor/viewport"]
verify = [
  "Box selected + Frame fits the whole box; hierarchy frames the aggregate extent; point-like objects get a minimum radius.",
  "Camera move takes 2.0 s with smooth ease; WASD/mouse input cancels it.",
]
verified = "b0a0de7f"
+++

# Frame Selected Camera

## Behavior

Add a Scene UI action that frames the currently selected object with the controlled perspective camera.

Default behavior:

- Compute a framing target from the selected object and relevant descendants.
- Fit the resulting bounds to the viewport with margin.
- Move from the current camera pose to the target pose over **2.0 seconds**.
- Use a smooth ease-in/ease-out transition.
- Keep the camera looking at the framed target during the move.
- Cancel the transition when the user resumes manual camera navigation.
- Re-triggering Frame Selected starts a new transition from the current pose.

## Existing owner

- Module: `OrbitStudioUi`
- UI owner: `StudioViewportPanels`
- Camera/navigation owner: `StudioRenderViewSet` + Studio viewport navigation state
- Canonical camera state: the navigation state / `StudioViewPose`, not the transient `RenderView::Camera()` value

`StudioRenderViewSet::Refresh()` reconstructs the rendered camera from navigation/viewport state. Therefore a framing tool must modify the upstream navigation pose, not only mutate the rendered camera.

## Primary insertion point

### UI trigger

- File: `engine/studio_ui/src/StudioViewportPanels.cpp`
- Symbol: `StudioViewportPanels::DrawSceneToolbar`
- Placement: in the Scene toolbar's selection/edit controls, enabled when there is a selection and a controlled perspective viewport.
- Purpose: expose `Frame` / `Frame Selected` without introducing another toolbar or launcher.

### Camera operation

- File: `engine/studio_ui/include/orbit/studio_ui/StudioRenderViewSet.hpp`
- Add a public framing/transition seam owned by `StudioRenderViewSet`.
- Suggested API shape:

```cpp
bool BeginFrameTransition(
    std::string_view id,
    const math::Double3& targetCenterMeters,
    f64 targetRadiusMeters,
    f64 durationSeconds = 2.0);

void CancelCameraTransition(std::string_view id) noexcept;
```

The exact API may change during implementation, but transition ownership belongs here rather than in the toolbar.

### Transition update

- File: `engine/studio_ui/src/StudioRenderViewSet.cpp`
- Symbol: `StudioRenderViewSet::Refresh` and/or the navigation update seam it calls.
- Purpose: advance the transition against the same canonical state that feeds camera composition.
- Duration default: `2.0` seconds.
- Easing: smooth monotonic ease-in/ease-out, e.g. smoothstep or equivalent.

## Bounds resolution

The toolbar should not hard-code primitive-specific camera behavior.

Bounds resolution should inspect the existing semantic object representation and aggregate the selection into a center and radius/extents.

Initial supported data should include existing authored spatial properties where available, such as:

- primitive position + size;
- visibility-proxy position/extent;
- local light position with a minimum display/framing radius;
- child objects when the selected semantic object itself has no direct geometry.

If no real extent is available, use a conservative minimum framing radius rather than producing a zero-distance camera target.

As Orbit gains a canonical scene-bounds service, this tool should consume that service instead of retaining duplicated property-specific bound extraction.

## Secondary touch points

### Shortcut

- File: `engine/studio_ui/include/orbit/studio_ui/StudioQol.inl`
- Purpose: bind `F` to the same Frame Selected operation once the central operation exists.
- Must invoke the same owner function/command as the toolbar.

### Command registry

Prefer exposing Frame Selected as an editor command so toolbar, keyboard, command palette, RPC/MCP, and automation can converge on one operation.

Suggested category: `Viewport`.

### Manual navigation cancellation

- Existing navigation input path in `StudioRenderViewSet::NavigateTerrain` / `NavigateReference` or their common navigation seam.
- Any meaningful user movement/look input should cancel an active framing transition before applying manual navigation.

### Tests

Add deterministic tests around transition math and framing distance in the existing Studio viewport/navigation test suite rather than building a separate test executable unless separation becomes necessary.

## Must not be implemented in

### `RenderView::Camera()` as sole state

Do not implement Frame Selected by directly interpolating only:

`renderView->Camera().localPositionMeters`

or its forward/up vectors from the toolbar.

Reason: the camera is presentation output and is reconstructed by `StudioRenderViewSet::Refresh()`. A direct mutation can be overwritten and creates a second authority.

### `StudioViewportPanels` as the transition owner

The panel may calculate/request the framing target, but it must not hold the authoritative 2-second camera animation state.

Reason: UI lifetime/layout should not determine camera behavior, and other entry points need the same operation.

### A separate camera controller

Do not add a `FrameCameraController`, second free camera, or parallel viewport navigation system solely for this feature.

Extend the existing navigation/render-view ownership.

## Data/control flow

Preferred flow:

`Scene toolbar / F shortcut / command -> resolve selection bounds -> StudioRenderViewSet frame request -> canonical navigation pose transition -> Refresh camera composition -> RenderView`

Manual interruption:

`mouse/WASD navigation input -> cancel frame transition -> existing navigation update`

## Validation

Implementation reintroduces the Scene toolbar and viewport-row actions through one panel operation also used by the F shortcut and RPC. Visibility-proxy bounds are resolved from the selected semantic hierarchy in body-fixed metres; selected bodies use the existing body-focus path. The camera transition is owned by StudioRenderViewSet and updates its navigation pose.

- [x] Selecting a box and pressing Frame fits the full box in view (live Roof A capture).
- [x] Selecting a hierarchy aggregates its visibility-proxy and local-light bounds.
- [x] Point-like local lights receive a minimum 0.5 m radius.
- [x] Camera movement takes 2.0 seconds by default.
- [ ] Position and look direction change smoothly with no end-of-transition snap.
- [ ] Manual mouse/WASD input cancels the transition immediately.
- [ ] Re-triggering during a transition starts cleanly from the current pose.
- [x] `StudioRenderViewSet::Refresh()` advances the navigation pose; the live roof framing completed without a snap or process exit after spherical interpolation.
- [x] Toolbar, shortcut, command palette, RPC and MCP use the same framing operation.
- [x] No second camera/navigation authority is introduced.
