# Live viewport manipulation

## Ownership and state

The viewport gizmo is owned by `StudioViewportManipulatorUi` in `engine/studio_ui`; the absolute transform calculation and undo transaction remain in `editor_model::ViewportManipulator`. A drag edits the selected object's properties in an open ObjectStore transaction. That transaction is authoritative for its in-progress preview, but its committed revision and undo entry must publish only on release.

## Refresh and camera stability

The ObjectStore keeps its committed semantic revision stable during the transaction and advances a separate monotonic preview revision on every mutation (including rollback restoration). The viewport renderer uses that preview revision to refresh authored proxy geometry from current in-transaction object properties without rebuilding celestial composition, invalidating the radiance cache every frame, or changing terrain navigation identity. On release, the gizmo acknowledges its render-only transform commit to the world session so it adopts the committed revision without needlessly rebuilding celestial composition. Cancel restores the object and triggers a preview refresh.

## Pointer capture

The Studio host supplies per-frame relative mouse movement to the gizmo. Starting a drag enables the platform's existing relative mouse mode (native cursor recenter/capture); a virtual viewport pointer accumulates deltas and may move beyond its visible bounds. Ending/cancelling the drag disables capture. This is an editor UI/native platform change and follows the automatic Studio-generation handoff path; no new extension or watcher classification is needed. The existing gizmo documentation and automated pointer input path remain available.
