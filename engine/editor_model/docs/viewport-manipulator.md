+++
path = "/editor/viewport/transform-gizmo"
title = "Viewport Move / Rotate / Scale gizmo"
kind = "subsystem"
status = "stable"
owner_module = "OrbitEditorModel"
summary = """
The Scene toolbar's Move / Rotate / Scale tools drive on-screen handles over the perspective viewport. All transform \
math and every write live in editor_model::ViewportManipulator (headless, tested); StudioViewportManipulatorUi only \
projects handles, hit-tests them and forwards pointer rays. A whole drag is one undo step, Esc cancels, and the same \
operation without a pointer is the object.transform RPC / orbit_object_transform MCP tool."""
keywords = ["gizmo", "move", "rotate", "scale", "translate", "handles", "manipulator", "transform", "drag", "snap", "viewport tools", "object.transform", "world local space"]
sources = [
  "engine/editor_model/include/orbit/editor_model/ViewportManipulator.hpp",
  "engine/editor_model/src/ViewportManipulator.cpp",
  "engine/editor_model/tests/ViewportManipulatorTests.cpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioViewportManipulatorUi.hpp",
  "engine/studio_ui/src/StudioViewportManipulatorUi.cpp",
  "engine/studio_ui/src/StudioViewportPanels.cpp",
  "engine/studio_ui/include/orbit/studio_ui/ViewportAuthoringState.hpp",
  "engine/editor_ui/src/EditorUi.cpp",
  "engine/render_view/src/RenderView.cpp",
  "engine/editor_rpc/src/EditorRpcService.cpp",
  "tools/mcp_server/orbit_editor_mcp_server.py",
  "apps/editor/src/Main.cpp",
]
symbols = ["ViewportManipulator", "ManipulatorTarget", "ResolveManipulatorTarget", "ManipulatorAxisDirection", "ApplyDelta", "StudioViewportManipulatorUi", "HandleViewportGizmo", "GizmoSettings", "ProjectToViewport", "OverlayShapesOnLastItem", "LastItemPointer"]
applies_to = ["engine/editor_model/src/ViewportManipulator.cpp", "engine/studio_ui/src/StudioViewportManipulatorUi.cpp"]
invariants = [
  "Transform math and writes live ONLY in editor_model::ViewportManipulator; the UI never computes a property value. The Scene toolbar sets GizmoSettings (tool, space, snap), the viewport handles feed pointer rays, and the object.transform RPC calls ApplyDelta, so a drag and an agent produce the same result.",
  "Supported objects: Primitive (move, rotate, scale), Visibility Proxy (move, rotate, scale: box half extents or sphere radius), Point Light (move), Spot Light (move, rotate its direction). Anything else has no handles (volumes, bodies, decals and reference nodes are not manipulable yet) and the viewport shows a hint instead of failing.",
  "One drag = one command transaction: Begin opens it, every Update writes the ABSOLUTE result for the current pointer (never incremental, so there is no drift), Commit is one undo step and Cancel/Esc rolls back to the exact start. Begin throws if another transaction is active.",
  "An active drag uses the ObjectStore preview revision to invalidate the proxy render scene each frame while the committed semantic revision stays unchanged. The final commit advances the committed revision once; rollback advances the preview revision to refresh the restored state. No universe identity change is published, so terrain navigation does not reset the camera.",
  "The Studio host enables platform relative-mouse capture only while a gizmo drag is active. The native cursor recenters and the gizmo accumulates relative deltas as an unbounded virtual viewport pointer, so movement keeps going beyond every screen edge. Capture is released on commit, cancel or target/world loss; standard camera right-drag retains its existing behavior.",
  "Drag math: translate = closest point on the handle axis to the pointer ray; rotate = angle swept in the plane normal to the axis; scale = ratio of axis parameters (uniform: ratio of distances on a plane perpendicular to the grab ray, so it is approximate in perspective). Rays that run parallel to a handle cannot grab it (Begin returns false).",
  "Spaces: Move and Rotate follow the toolbar's World/Local choice; Scale ALWAYS uses the object's own axes (EffectiveSpace). Euler angles use the intrinsic XYZ convention (Rz * Ry * Rx) shared with the renderer; rotation is composed as a matrix and converted back, folding roll into X at gimbal lock.",
  "Property space: authored positions, directions and Euler angles are treated as living in the viewport's local frame (camera.localPositionMeters frame), the same assumption the light and volume gizmos make. A parent frame with its own rotation is not accounted for yet.",
  "Snap comes from the toolbar settings: translate snaps the displacement from the drag start, rotate snaps the swept angle, scale snaps the factor to steps and never below one step. Surface snap and pivot modes are stored in GizmoSettings but not used (single selection only).",
  "Handles keep a constant on-screen size (about 110 px, scaled by the UI scale) by converting pixels to meters at the object's depth; they are 2D ImGui overlays drawn after the viewport Image (OverlayShapesOnLastItem), so handle drawing never depends on GPU resources and the overlay survives generation reloads.",
  "While the handles own the left button (hovering one or dragging) HandleViewportGizmo returns true and the caller must not treat the press as a selection, terrain pick or path-placement click. A drag is cancelled when the selection changes or the authoring world is replaced.",
  "The handles draw only for a single selected object in a Perspective-mode viewport; the Body Map and Debug views have no 3D camera to project with.",
]
related = ["/editor/viewport", "/editor/mcp-rpc", "/editor/studio-ui", "/foundation/rpc"]
depends_on = ["/editor/viewport", "/editor/model"]
used_by = ["/editor/viewport"]
verify = [
  "ctest -R Orbit.ViewportManipulator (translate / rotate / scale math, snap, one-undo-step, cancel, local axes, proxies, lights, ApplyDelta) and Orbit.ViewRay (ProjectToViewport inverts ViewportRay).",
  "ctest -R Orbit.EditorRpc: object.transform / object.transform_info (one undo step per call, -32602 on bad tool/axis/amount or an unsupported object).",
  "Interactive (Windows only, not covered by tests): select a Primitive / Visibility Proxy / light, pick Move, Rotate or Scale in the Scene toolbar, drag a handle, press Esc mid-drag (object returns), Undo after a drag (one step).",
]
verified = "55d48117"

[routes]
"drive the transform without a pointer" = "/editor/mcp-rpc"

[[diagnose]]
symptom = "no handles appear on the selected object"
steps = [
  "The Scene toolbar tool must be Move, Rotate or Scale (Select draws nothing) and exactly one object must be selected.",
  "Call object.transform_info {object}: it lists the supported tools; an object type without transform properties (volume, body, decal, reference node) has no handles, and a Point Light has no rotate/scale.",
  "The viewport must be in Perspective mode and the object in front of the camera (ProjectToViewport returns nothing behind it).",
  "Primitives and Visibility Proxies are not rendered as meshes yet (docs/ORBIT_PRIMITIVES.md), so the handles can sit on an invisible object; lights show their own range/cone lines.",
]
docs = ["/editor/viewport"]

[[diagnose]]
symptom = "dragging a handle does nothing, jumps, or selects the body instead"
steps = [
  "'That handle cannot be grabbed from this angle' means the view ray runs parallel to the handle axis or plane; orbit the camera so the handle is not edge-on.",
  "A press that selects the body means the handle did not claim it: check HandleViewportGizmo is called right after the viewport Image (it reads the last ImGui item) and its result suppresses interaction.clicked in the caller (apps/editor/src/Main.cpp and StudioViewportPanels.cpp).",
  "Jumps near an object far from the origin are a precision symptom of the local frame: positions are camera-local doubles, so check the object's frame assumption (property space note above).",
  "Reproduce headlessly with object.transform (same code path) to separate input handling from math.",
]
docs = ["/editor/mcp-rpc"]
+++

## Pieces

| Piece | Owner |
| --- | --- |
| tool / space / snap state, toolbar | `GizmoSettings` (`ViewportAuthoringState.hpp`), `StudioViewportPanels::DrawSceneToolbar` |
| handle projection, hit test, overlay | `StudioViewportManipulatorUi::Handle` |
| overlay drawing and pointer reading | `PanelContext::OverlayShapesOnLastItem`, `OverlayLabelOnLastItem`, `LastItemPointer` |
| world to screen | `render_view::ProjectToViewport` (inverse of `ViewportRay`) |
| math and writes | `editor_model::ViewportManipulator` |
| headless entry | `object.transform` / `object.transform_info` RPC, `orbit_object_transform*` MCP tools |

## Controls

Left-drag a coloured axis arrow (Move), ring (Rotate) or axis dot (Scale; the circle at the centre scales uniformly).
The object follows the drag continuously while the camera stays put. The pointer is captured and loops through the
viewport so the drag can continue indefinitely. Esc cancels the drag; Undo reverts a finished drag in one step.
Right-drag and the keyboard still navigate the camera.

## Hot iteration

All of it is ordinary engine source: saving it takes the automatic native/generation handoff (nothing to restart). The
handles are CPU-side ImGui overlays, so no GPU resource belongs to this feature and a failed hot build leaves the running
generation (and any drag in progress) alive.
